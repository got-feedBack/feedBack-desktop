#pragma once

// RendererBus — the WebAudio→engine audio bus (TLC plan phase 2 / §2.6).
// Moved verbatim from AudioEngine (see git history for the original inline
// comments' evolution): the renderer pushes its WebAudio master mix here over
// IPC so song/stem audio stays audible when the output device is
// exclusive-style (ASIO / WASAPI exclusive) and the OS mixer path is silent.
//
// SPSC: producer is the main-process IPC thread (push — includes the linear
// resampler), consumer is whichever output callback is live (pull). Sized
// generously (~1.5 s @ 48 kHz) because the producer has scheduling jitter;
// the consumer trims steady-state fill via the fill clamp.
//
// JUCE-free on purpose: pull() takes raw channel pointers, so
// tests/engine_units drives the resampler/prime/clamp logic without a device.

#include "PackedStereoRing.h"
#include "../GainSanitize.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace slopsmith {

class RendererBus
{
public:
    static constexpr int kFrames = 65536;
    // Prefill gate: consume nothing until the producer has built this cushion
    // (~10.7 ms @ 48 kHz); re-armed after every underflow so stall recovery is
    // one clean gap. Fill clamp: fill beyond kMaxFillFrames (~85 ms) means a
    // renderer stall dumped a backlog — trim to the prime target, don't play
    // the tail.
    static constexpr int kPrimeFrames   = 512;
    static constexpr int kMaxFillFrames = 4096;

    void setEnabled(bool enabled, float gain)
    {
        busGain.store(sanitizeStreamGain(gain), std::memory_order_relaxed);
        const bool was = busEnabled.exchange(enabled, std::memory_order_acq_rel);
        if (was && !enabled)
        {
            // Drop buffered audio on disable so a later re-enable starts fresh
            // instead of playing a stale tail. The CONSUMER honors this flag at
            // its next pull (deep-read §4 fix): the old control-thread write to
            // readIndex violated the ring's own SPSC discipline — a concurrent
            // pull mid-drain could overwrite it with r + pull, replaying a
            // stale tail after re-enable, exactly what the drop was meant to
            // prevent. Only the consumer ever moves readIndex now.
            flushRequested.store(true, std::memory_order_release);
            primed.store(false, std::memory_order_relaxed);
        }
    }
    bool isEnabled() const { return busEnabled.load(std::memory_order_relaxed); }

    // Interleaved stereo frames at `sourceRate`, linear-resampled to
    // `deviceRate` on the producer thread (fractional position + previous
    // frame carried across calls). Returns false when the bus is disabled or
    // the rates are unusable. Drop-oldest on overflow, counted consumer-side.
    bool push(const float* interleavedLR, int frames, double sourceRate, double deviceRate)
    {
        if (!busEnabled.load(std::memory_order_acquire)) return false;
        if (interleavedLR == nullptr || frames <= 0) return false;
        // Both rates cross the JS/IPC boundary: reject NaN/Inf (a NaN
        // deviceRate passes a plain `<= 0.0` check) and a step that
        // underflowed to zero (subnormal source rate), either of which would
        // make the resample loop index garbage or never advance.
        if (!std::isfinite(deviceRate) || deviceRate <= 0.0) return false;
        if (!std::isfinite(sourceRate) || sourceRate <= 0.0) sourceRate = deviceRate;

        uint64_t w = ring.beginWrite();

        // Linear resample source→device rate on this (IPC) thread. `pos` is
        // the fractional read position into the incoming chunk; index -1
        // refers to the carried last frame of the previous chunk so
        // interpolation is continuous across pushes. Equal rates degenerate
        // to step == 1.0 (still exact: pos stays integral, frac == 0).
        const double step = sourceRate / deviceRate;
        if (!std::isfinite(step) || step <= 0.0) return false;
        double pos = srcPos;
        uint64_t written = 0;
        while (true)
        {
            const double ip = std::floor(pos);
            const int i0 = (int) ip;
            if (i0 + 1 >= frames) break;             // next chunk continues from here
            const float frac = (float) (pos - ip);
            const float l0 = (i0 < 0) ? prevL : interleavedLR[(size_t) i0 * 2];
            const float r0 = (i0 < 0) ? prevR : interleavedLR[(size_t) i0 * 2 + 1];
            const float l1 = interleavedLR[((size_t) i0 + 1) * 2];
            const float r1 = interleavedLR[((size_t) i0 + 1) * 2 + 1];
            ring.stageFrame(w, l0 + (l1 - l0) * frac, r0 + (r1 - r0) * frac);
            ++w;
            ++written;
            pos += step;
        }
        srcPos = pos - (double) frames;              // relative to the next chunk
        prevL = interleavedLR[((size_t) frames - 1) * 2];
        prevR = interleavedLR[((size_t) frames - 1) * 2 + 1];

        // Publish. Overflow (producer lapping the consumer) is handled
        // consumer-side with drop-oldest — only the consumer moves readIndex.
        ring.publish(w);
        pushedFrames.fetch_add(written, std::memory_order_relaxed);
        return true;
    }

    // Drain one block into dl/dr (bus gain applied). Returns numSamples on
    // success, 0 when gated (disabled, priming, underflow). Single consumer —
    // call exactly once per output block.
    int pull(float* dl, float* dr, int numSamples)
    {
        // Consume a pending flush FIRST — even while disabled — so the tail
        // buffered before a disable is dropped by the ring's one legitimate
        // readIndex writer (this consumer), never by the control thread.
        if (flushRequested.exchange(false, std::memory_order_acq_rel))
            ring.commitRead(ring.writeIndex.load(std::memory_order_acquire));
        if (!busEnabled.load(std::memory_order_acquire)) return 0;
        const uint64_t w = ring.writeIndex.load(std::memory_order_acquire);
        uint64_t r = ring.readIndex.load(std::memory_order_relaxed);
        if (w - r > (uint64_t) kFrames)
        {
            // Producer lapped us — drop-oldest to the newest full ring.
            r = w - (uint64_t) kFrames;
            overflowCount.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t avail = w - r;

        // Fill clamp (spike finding): steady-state drift is near zero, so a
        // fill beyond kMaxFillFrames only ever means a renderer stall dumped a
        // backlog. Trim to the prime target instead of playing the whole tail
        // at ~85+ ms behind — a latency reset, not an audible gap.
        if (avail > (uint64_t) kMaxFillFrames)
        {
            r = w - (uint64_t) kPrimeFrames;
            avail = (uint64_t) kPrimeFrames;
            overflowCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Prefill gate (spike finding): the warmup underflow burst is the mix
        // starting before the ring has a cushion. Consume nothing until the
        // producer has built ~10 ms; re-arm the same gate after a real
        // underflow so stall recovery is one clean gap, not a ragged refill.
        if (!primed)
        {
            if (avail < (uint64_t) kPrimeFrames)
            {
                ring.commitRead(r);
                return 0;
            }
            primed = true;
        }
        if (avail < (uint64_t) numSamples)
        {
            // Underflow: emit silence for the whole block (partial blocks
            // blip), drop what's buffered, and go back to priming.
            primed = false;
            underflowCount.fetch_add(1, std::memory_order_relaxed);
            ring.commitRead(w);
            return 0;
        }

        const float g = busGain.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            float l, rr;
            ring.readFrame(r + (uint64_t) i, l, rr);
            dl[i] = l * g;
            dr[i] = rr * g;
        }
        ring.commitRead(r + (uint64_t) numSamples);
        consumedFrames.fetch_add((uint64_t) numSamples, std::memory_order_relaxed);
        return numSamples;
    }

    struct Metrics
    {
        uint64_t pushedFrames = 0, consumedFrames = 0, underflowCount = 0, overflowCount = 0;
        int fillFrames = 0, capacityFrames = 0;
        bool enabled = false;
    };
    Metrics metrics() const
    {
        Metrics m;
        m.pushedFrames   = pushedFrames.load(std::memory_order_relaxed);
        m.consumedFrames = consumedFrames.load(std::memory_order_relaxed);
        m.underflowCount = underflowCount.load(std::memory_order_relaxed);
        m.overflowCount  = overflowCount.load(std::memory_order_relaxed);
        const uint64_t w = ring.writeIndex.load(std::memory_order_acquire);
        const uint64_t r = ring.readIndex.load(std::memory_order_acquire);
        const uint64_t fill = w - r;
        m.fillFrames = (int) (fill < (uint64_t) kFrames ? fill : (uint64_t) kFrames);
        m.capacityFrames = kFrames;
        m.enabled = busEnabled.load(std::memory_order_relaxed);
        return m;
    }

private:
    PackedStereoRing<kFrames> ring;
    std::atomic<uint64_t> pushedFrames{0};
    std::atomic<uint64_t> consumedFrames{0};
    std::atomic<uint64_t> underflowCount{0};
    std::atomic<uint64_t> overflowCount{0};
    std::atomic<bool>  busEnabled{false};
    std::atomic<float> busGain{1.0f};
    // Consumer-side prefill-gate state. Only the live output callback touches
    // it, but duplex/split hand-offs cross threads — atomic keeps that safe.
    std::atomic<bool> primed{false};
    // Set by setEnabled(false) on the control thread, consumed (exchange) by
    // pull() — the drop-on-disable request, honored by the single consumer.
    std::atomic<bool> flushRequested{false};
    // Producer-thread-only linear-resampler state (fractional read position
    // into the incoming chunk + the previous chunk's last frame for
    // interpolation continuity across pushes).
    double srcPos = 0.0;
    float  prevL = 0.0f, prevR = 0.0f;
};

} // namespace slopsmith
