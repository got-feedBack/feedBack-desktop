#pragma once

// PackedStereoRing — the ONE packed-LR SPSC ring (audio-engine TLC, plan
// phase 1). Replaces the three hand-maintained copies of the same design:
// the split-mode outputPendingRing, each InputDeviceSlot's ring, the stream
// sink's ring, and the renderer-audio bus ring.
//
// Design (moved verbatim from AudioEngine.h — see git history for the
// original per-site comments):
//
// Each slot packs one stereo frame (L+R floats) into a single 64-bit atomic
// so the consumer reads both channels in one indivisible load — without
// packing, the producer's two separate atomic stores could interleave with
// the consumer's two loads during a drop-oldest wrap, surfacing as
// L_new+R_old (or vice versa) sample tears.
//
// Strict SPSC: the producer (one device/IPC thread) only ever writes
// writeIndex; the consumer is the sole writer of readIndex. Drop-oldest =
// letting writeIndex lap the buffer; the consumer advances readIndex when it
// observes (w - r) > capacity. Ordering is established by the release store
// on writeIndex (producer) / readIndex (consumer); slot stores/loads are
// relaxed.
//
// The indices and slots are deliberately PUBLIC: the renderer bus keeps its
// bespoke prefill-gate / fill-clamp consumer policy, and phase-2 units bind
// the members directly. The helpers below own only the ritual moves every
// site repeats: producer publish, reset, the w<r resync after an index
// reset, and the lapped catch-up with its overflow counter.

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace slopsmith {

// Pack/unpack helpers — std::bit_cast (C++20) is constexpr + alias-safe.
inline uint64_t packLR(float l, float r) noexcept
{
    const uint32_t li = std::bit_cast<uint32_t>(l);
    const uint32_t ri = std::bit_cast<uint32_t>(r);
    return (static_cast<uint64_t>(ri) << 32) | static_cast<uint64_t>(li);
}
inline void unpackLR(uint64_t v, float& l, float& r) noexcept
{
    l = std::bit_cast<float>(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    r = std::bit_cast<float>(static_cast<uint32_t>(v >> 32));
}

template <int NFrames>
struct PackedStereoRing
{
    static_assert((NFrames & (NFrames - 1)) == 0,
                  "ring capacity must be a power of two for mask wraparound");
    // RT-thread reads + writes touch these slots, so a lock-based fallback
    // would risk priority inversion + audible dropouts. On the platforms we
    // ship (x86_64 + arm64 across Linux/macOS/Windows) atomic<uint64_t> is
    // always lock-free; this assert turns a regression into a build error
    // instead of a silent latency degradation if a future platform port
    // breaks the assumption.
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "PackedStereoRing requires lock-free atomic<uint64_t> for RT safety");
    static_assert(sizeof(float) == 4, "pack/unpack assumes 32-bit float");

    static constexpr uint64_t kMask = (uint64_t) NFrames - 1;
    static constexpr uint64_t kCap  = (uint64_t) NFrames;
    static constexpr int      kFrames = NFrames;

    std::array<std::atomic<uint64_t>, NFrames> slots{};
    std::atomic<uint64_t> writeIndex{0};
    std::atomic<uint64_t> readIndex{0};

    // ── Producer side ─────────────────────────────────────────────────────
    // Publish a stereo block (drop-oldest by lapping; consumer catches up).
    void push(const float* L, const float* R, int numSamples) noexcept
    {
        const uint64_t w = writeIndex.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
            slots[(size_t) ((w + (uint64_t) i) & kMask)].store(packLR(L[i], R[i]),
                                                               std::memory_order_relaxed);
        writeIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
    }
    // Frame-at-a-time producer path (renderer-bus resampler): stage frames at
    // monotonically increasing indices from beginWrite(), then publish once.
    uint64_t beginWrite() const noexcept { return writeIndex.load(std::memory_order_relaxed); }
    void stageFrame(uint64_t index, float l, float r) noexcept
    {
        slots[(size_t) (index & kMask)].store(packLR(l, r), std::memory_order_relaxed);
    }
    void publish(uint64_t newWriteIndex) noexcept
    {
        writeIndex.store(newWriteIndex, std::memory_order_release);
    }

    // ── Consumer side ─────────────────────────────────────────────────────
    void readFrame(uint64_t index, float& l, float& r) const noexcept
    {
        unpackLR(slots[(size_t) (index & kMask)].load(std::memory_order_relaxed), l, r);
    }
    void commitRead(uint64_t newReadIndex) noexcept
    {
        readIndex.store(newReadIndex, std::memory_order_release);
    }
    // The two ritual guards at the top of every drain, exactly as each site
    // wrote them by hand. `r` is the consumer's working copy of readIndex.
    //
    // If a stop/reset raced between the consumer's two index loads and reset
    // both indices to 0, the consumer can observe w < r. Treat that as an
    // empty ring and resync — without this, the unsigned (w - r) wraps into a
    // huge positive value and falls into the catch-up branch reading stale
    // slots.
    void resyncIfIndicesReset(uint64_t& r, uint64_t w) noexcept
    {
        if (w < r) { r = w; readIndex.store(r, std::memory_order_relaxed); }
    }
    // Catch up if the producer has lapped (drop-oldest is achieved via this
    // single-writer consumer-side advance, not a producer-side write to r).
    // Returns true when a lap was consumed so the caller can bump its counter.
    bool catchUpIfLapped(uint64_t& r, uint64_t w) noexcept
    {
        if ((w - r) > kCap)
        {
            r = w - kCap;
            readIndex.store(r, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // ── Lifecycle (control/device-management threads only) ────────────────
    void resetIndices() noexcept
    {
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
    }
    void reset() noexcept
    {
        resetIndices();
        for (auto& v : slots) v.store(0, std::memory_order_relaxed);
    }
};

} // namespace slopsmith
