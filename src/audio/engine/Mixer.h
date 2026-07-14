#pragma once

// Mixer — the engine-owned channel mixer (docs/audio-ownership-plan.md §5).
// Every audible thing becomes a channel: channel #0 is the permanent default
// (the renderer master via loopback capture — the old RendererBus,
// byte-compatible), bespoke channels are the opt-in upgrade for producers
// that want their own gain/mute/meter/diagnostics.
//
// Threading model (same discipline as RendererBus):
//   - producer side (push):     main-process IPC thread, per channel (SPSC)
//   - consumer side (pullMix):  the live output callback, all channels
//   - control side (create/release/gain/mute): main-process control thread
//
// Slot lifecycle is a lock-free state machine so the audio thread never
// touches a slot being torn down:
//   Free ──create()──▶ Active ──release()──▶ Draining ──audio thread fades
//   to silence over one block──▶ Free  (or control-side Free when no
//   consumer is running). Reclaim is a fade, never a click (§5).
//
// Channel groups (§8.13): a group is a shared START gate at block
// granularity — no member is pulled until every member in the group has
// audio buffered, so grouped producers begin in the same output block.
// Ungrouped channels stay fully independent streams.
//
// JUCE-free on purpose, same as RendererBus: tests/engine_units drives the
// mix/fade/group logic without a device.

#include "RendererBus.h"
#include "../GainSanitize.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace slopsmith {

class Mixer
{
public:
    // Hard cap (§8.9): channels are cheap but not free — each is a ring +
    // resampler + one mix iteration per callback. Refusal at the cap is
    // `no-capacity`; idle reaping lives JS-side on the metrics.
    static constexpr int kMaxChannels = 24;
    static constexpr int kLabelMax  = 64;
    static constexpr int kHolderMax = 128;
    static constexpr int kKindMax   = 16;

    enum class SlotState : uint32_t { Free = 0, Active = 1, Draining = 2 };

    Mixer()
    {
        // Channel #0: the permanent default (plan §5 decision) — always
        // present, never reaped, never released.
        auto& s = slots[0];
        s.bus = std::make_unique<RendererBus>();
        copyBounded(s.label, kLabelMax, "renderer-master");
        copyBounded(s.holder, kHolderMax, "engine:renderer-default");
        copyBounded(s.kind, kKindMax, "default");
        s.state.store((uint32_t) SlotState::Active, std::memory_order_release);
        channelCount.store(1, std::memory_order_relaxed);
    }

    // The default channel's bus — the byte-compatible RendererBus surface
    // (setRendererBus / pushRendererAudio / getRendererBusMetrics delegate
    // here so the existing renderer-bus suite passes unchanged).
    RendererBus& defaultBus() { return *slots[0].bus; }
    const RendererBus& defaultBus() const { return *slots[0].bus; }

    // ── control thread ───────────────────────────────────────────────────────

    // Returns the channel id, or -1 at the cap (`no-capacity`). `consumerLive`
    // tells us whether an output callback exists to complete Draining→Free;
    // without one the control thread reclaims Draining slots directly.
    int createChannel(const char* label, const char* kind, const char* holder, bool consumerLive)
    {
        for (int i = 1; i < kMaxChannels; ++i)
        {
            auto& s = slots[i];
            // Reclaim a parked Draining slot when no consumer will ever fade it.
            uint32_t draining = (uint32_t) SlotState::Draining;
            if (!consumerLive)
                s.state.compare_exchange_strong(draining, (uint32_t) SlotState::Free,
                                                std::memory_order_acq_rel);
            uint32_t expected = (uint32_t) SlotState::Free;
            if (s.state.compare_exchange_strong(expected, (uint32_t) SlotState::Active,
                                                std::memory_order_acq_rel))
            {
                // Ring allocated lazily on this control thread (~512 KB per
                // channel), kept for the process lifetime once claimed
                // (high-water): the audio thread may hold a reference between
                // our state checks, so a slot's bus is never deallocated. The
                // Active store above happens-before any audio-thread read of
                // the pointer via the acquire load in pullMixInto.
                if (!s.bus) s.bus = std::make_unique<RendererBus>();
                copyBounded(s.label, kLabelMax, label);
                copyBounded(s.kind, kKindMax, kind);
                copyBounded(s.holder, kHolderMax, holder);
                s.gain.store(1.0f, std::memory_order_relaxed);
                s.mute.store(false, std::memory_order_relaxed);
                s.group.store(-1, std::memory_order_relaxed);
                s.generation.fetch_add(1, std::memory_order_relaxed);
                s.bus->setEnabled(true, 1.0f);
                channelCount.fetch_add(1, std::memory_order_relaxed);
                return i;
            }
        }
        return -1;
    }

    // Fade-to-silence release (§5 reclaim). Channel 0 is never releasable.
    bool releaseChannel(int id, bool consumerLive)
    {
        if (id <= 0 || id >= kMaxChannels) return false;
        auto& s = slots[id];
        uint32_t expected = (uint32_t) SlotState::Active;
        if (!s.state.compare_exchange_strong(expected, (uint32_t) SlotState::Draining,
                                             std::memory_order_acq_rel))
            return false;
        // Producer pushes stop immediately (pushChannel only accepts Active
        // slots). The bus stays ENABLED through the drain so the audio thread
        // can pull one last block and fade it — disabling here would flush
        // the tail and cut mid-waveform (a click, exactly what §5 forbids).
        channelCount.fetch_sub(1, std::memory_order_relaxed);
        if (!consumerLive)
        {
            // No output callback running — nothing to fade; flush and free.
            s.bus->setEnabled(false, 0.0f);
            s.state.store((uint32_t) SlotState::Free, std::memory_order_release);
        }
        return true;
    }

    bool setChannelGain(int id, float gain)
    {
        auto* s = activeSlot(id);
        if (s == nullptr) return false;
        // Native clamp (§5.1 tier 2 — no JS-side trust), same sanitizer as
        // the stream gain path.
        s->gain.store(sanitizeStreamGain(gain), std::memory_order_relaxed);
        return true;
    }

    bool setChannelMute(int id, bool mute)
    {
        auto* s = activeSlot(id);
        if (s == nullptr) return false;
        s->mute.store(mute, std::memory_order_relaxed);
        return true;
    }

    bool setChannelGroup(int id, int group)
    {
        auto* s = activeSlot(id);
        if (s == nullptr) return false;
        s->group.store(group < 0 ? -1 : group, std::memory_order_relaxed);
        return true;
    }

    // Producer push for bespoke channels (tier 3). Channel 0 pushes ride the
    // legacy pushRendererAudio path onto the same bus.
    bool pushChannel(int id, const float* interleavedLR, int frames, double sourceRate, double deviceRate)
    {
        auto* s = activeSlot(id);
        if (s == nullptr) return false;
        return s->bus->push(interleavedLR, frames, sourceRate, deviceRate);
    }

    // ── audio thread ─────────────────────────────────────────────────────────

    // Mix every ready channel into dl/dr (adding), using the caller's scratch
    // (never allocates). Returns the number of channels that contributed
    // audio this block. Per-channel gain is ramped across the block so gain
    // steps, mutes, and the draining fade are click-free.
    int pullMixInto(float* dl, float* dr, int numSamples, float* scratchL, float* scratchR)
    {
        if (numSamples <= 0) return 0;

        // Group start gates (§8.13): a group is ready once every Active
        // member has at least a prime cushion buffered. Bitmask per group id
        // 0..31; ids beyond that are treated as ungrouped.
        uint32_t groupBlocked = 0;
        for (int i = 0; i < kMaxChannels; ++i)
        {
            auto& s = slots[i];
            if (s.state.load(std::memory_order_acquire) != (uint32_t) SlotState::Active) continue;
            const int g = s.group.load(std::memory_order_relaxed);
            if (g < 0 || g > 31) continue;
            if (s.bus->metrics().fillFrames < RendererBus::kPrimeFrames)
                groupBlocked |= (1u << g);
        }

        int contributed = 0;
        for (int i = 0; i < kMaxChannels; ++i)
        {
            auto& s = slots[i];
            const uint32_t state = s.state.load(std::memory_order_acquire);
            if (state == (uint32_t) SlotState::Free) continue;

            if (state == (uint32_t) SlotState::Draining)
            {
                // One-block fade from the last smoothed gain to zero, then
                // disable (flushes the remaining tail) and Free. setEnabled
                // from this thread is safe — it only stores atomics, and the
                // producer stopped pushing when the slot left Active.
                const int n = s.bus->pull(scratchL, scratchR, numSamples);
                if (n > 0 && s.smoothedGain > 0.0f)
                {
                    rampInto(dl, dr, scratchL, scratchR, n, s.smoothedGain, 0.0f);
                    ++contributed;
                }
                s.smoothedGain = 0.0f;
                s.bus->setEnabled(false, 0.0f);
                s.state.store((uint32_t) SlotState::Free, std::memory_order_release);
                continue;
            }

            const int g = s.group.load(std::memory_order_relaxed);
            if (g >= 0 && g <= 31 && (groupBlocked & (1u << g)) != 0)
                continue; // group not ready — member keeps buffering

            const int n = s.bus->pull(scratchL, scratchR, numSamples);
            if (n <= 0)
            {
                // No audio this block; keep the smoothed gain tracking so a
                // later resume doesn't ramp from an ancient value.
                s.smoothedGain = targetGainOf(s);
                continue;
            }
            const float target = targetGainOf(s);
            rampInto(dl, dr, scratchL, scratchR, n, s.smoothedGain, target);
            s.smoothedGain = target;
            ++contributed;
        }
        return contributed;
    }

    // ── diagnostics (any thread) ────────────────────────────────────────────

    struct ChannelInfo
    {
        int id = -1;
        char label[kLabelMax] = {};
        char kind[kKindMax] = {};
        char holder[kHolderMax] = {};
        float gain = 1.0f;
        bool mute = false;
        int group = -1;
        RendererBus::Metrics metrics;
    };

    // Fills `out` (size kMaxChannels), returns count of non-Free channels.
    int listChannels(ChannelInfo* out) const
    {
        int count = 0;
        for (int i = 0; i < kMaxChannels; ++i)
        {
            const auto& s = slots[i];
            if (s.state.load(std::memory_order_acquire) == (uint32_t) SlotState::Free) continue;
            auto& info = out[count++];
            info.id = i;
            std::memcpy(info.label, s.label, kLabelMax);
            std::memcpy(info.kind, s.kind, kKindMax);
            std::memcpy(info.holder, s.holder, kHolderMax);
            info.gain = s.gain.load(std::memory_order_relaxed);
            info.mute = s.mute.load(std::memory_order_relaxed);
            info.group = s.group.load(std::memory_order_relaxed);
            info.metrics = s.bus->metrics();
        }
        return count;
    }

    int activeChannelCount() const { return channelCount.load(std::memory_order_relaxed); }

private:
    struct Slot
    {
        // Lazily allocated (~512 KB ring) on first claim, then kept for the
        // process lifetime — see createChannel. Only slot 0 exists up front.
        std::unique_ptr<RendererBus> bus;
        std::atomic<uint32_t> state{(uint32_t) SlotState::Free};
        std::atomic<float> gain{1.0f};
        std::atomic<bool> mute{false};
        std::atomic<int> group{-1};
        std::atomic<uint32_t> generation{0};
        char label[kLabelMax] = {};
        char holder[kHolderMax] = {};
        char kind[kKindMax] = {};
        // Audio-thread-only ramp state (click-free gain steps / mute / fade).
        float smoothedGain = 1.0f;
    };

    Slot* activeSlot(int id)
    {
        if (id < 0 || id >= kMaxChannels) return nullptr;
        auto& s = slots[id];
        if (s.state.load(std::memory_order_acquire) != (uint32_t) SlotState::Active) return nullptr;
        return &s;
    }

    static float targetGainOf(const Slot& s)
    {
        return s.mute.load(std::memory_order_relaxed)
            ? 0.0f
            : s.gain.load(std::memory_order_relaxed);
    }

    // Add scratch into dl/dr with a linear gain ramp from `from` to `to`
    // across the block.
    static void rampInto(float* dl, float* dr, const float* sl, const float* sr,
                         int n, float from, float to)
    {
        if (n <= 0) return;
        const float stepG = (to - from) / (float) n;
        float g = from;
        for (int i = 0; i < n; ++i)
        {
            g += stepG;
            dl[i] += sl[i] * g;
            dr[i] += sr[i] * g;
        }
    }

    static void copyBounded(char* dst, int cap, const char* src)
    {
        if (src == nullptr) { dst[0] = '\0'; return; }
        int i = 0;
        for (; i < cap - 1 && src[i] != '\0'; ++i) dst[i] = src[i];
        dst[i] = '\0';
    }

    Slot slots[kMaxChannels];
    std::atomic<int> channelCount{0};
};

} // namespace slopsmith
