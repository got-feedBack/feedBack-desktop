// Phase 1 unit tests for PackedStereoRing (docs/audio-engine-tlc.md §5):
// pack/unpack round-trip, wrap + drop-oldest lap, w<r resync after an index
// reset, L/R tear check under a concurrent producer lapping the consumer,
// and the pull-vs-consume skew pattern the split output path relies on.

#include "../../src/audio/engine/PackedStereoRing.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>

using slopsmith::PackedStereoRing;
using slopsmith::packLR;
using slopsmith::unpackLR;

static void testPackRoundTrip()
{
    const float values[] = { 0.0f, -0.0f, 1.0f, -1.0f, 3.14159f, 1e-30f, -1e30f };
    for (float l : values)
        for (float r : values)
        {
            float ol, orr;
            unpackLR(packLR(l, r), ol, orr);
            // Bit-exact round trip (including -0.0f).
            assert(std::memcmp(&ol, &l, 4) == 0 && std::memcmp(&orr, &r, 4) == 0);
        }
}

static void testPushPullBasic()
{
    PackedStereoRing<64> ring;
    float L[16], R[16];
    for (int i = 0; i < 16; ++i) { L[i] = (float) i; R[i] = (float) -i; }
    ring.push(L, R, 16);

    uint64_t r = ring.readIndex.load();
    const uint64_t w = ring.writeIndex.load();
    assert(w - r == 16);
    for (int i = 0; i < 16; ++i)
    {
        float l, rr;
        ring.readFrame(r + (uint64_t) i, l, rr);
        assert(l == (float) i && rr == (float) -i);
    }
    ring.commitRead(r + 16);
    assert(ring.writeIndex.load() - ring.readIndex.load() == 0);
}

static void testLapCatchUp()
{
    PackedStereoRing<64> ring;
    float L[64], R[64];
    // Push 3 laps' worth without consuming: consumer must catch up to newest
    // full ring, exactly once per drain regardless of how far it was lapped.
    for (int block = 0; block < 3; ++block)
    {
        for (int i = 0; i < 64; ++i) { L[i] = (float) (block * 64 + i); R[i] = 0.0f; }
        ring.push(L, R, 64);
    }
    uint64_t r = ring.readIndex.load();
    const uint64_t w = ring.writeIndex.load();
    assert(w - r == 192);
    const bool lapped = ring.catchUpIfLapped(r, w);
    assert(lapped);
    assert(w - r == 64); // newest full ring only
    float l, rr;
    ring.readFrame(r, l, rr);
    assert(l == 128.0f); // oldest surviving frame = start of last lap
    // Not lapped anymore: second call is a no-op.
    assert(!ring.catchUpIfLapped(r, w));
}

static void testResyncAfterReset()
{
    PackedStereoRing<64> ring;
    float L[32] = {}, R[32] = {};
    ring.push(L, R, 32);
    ring.commitRead(20);
    uint64_t r = ring.readIndex.load();
    // A stop raced in and reset the indices; consumer still holds r == 20.
    ring.resetIndices();
    const uint64_t w = ring.writeIndex.load();
    ring.resyncIfIndicesReset(r, w);
    assert(r == 0 && w == 0); // treated as empty, no wrapped (w - r) monster
}

// Producer at one block size laps a slower consumer at another; every frame
// the consumer reads must have L == -R (the producer invariant), proving the
// packed single-atomic store never tears a frame.
static void testConcurrentTearFreedom()
{
    PackedStereoRing<256> ring;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> laps{0};

    std::thread producer([&] {
        float L[48], R[48];
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            for (int i = 0; i < 48; ++i)
            {
                const float v = (float) ((n + (uint64_t) i) & 0xFFFFF);
                L[i] = v; R[i] = -v;
            }
            ring.push(L, R, 48);
            n += 48;
        }
    });

    uint64_t checked = 0;
    while (checked < 2'000'000)
    {
        uint64_t r = ring.readIndex.load(std::memory_order_relaxed);
        const uint64_t w = ring.writeIndex.load(std::memory_order_acquire);
        ring.resyncIfIndicesReset(r, w);
        if (ring.catchUpIfLapped(r, w)) laps.fetch_add(1);
        const uint64_t avail = w - r;
        const int pull = (int) (avail < 32 ? avail : 32);
        for (int i = 0; i < pull; ++i)
        {
            float l, rr;
            ring.readFrame(r + (uint64_t) i, l, rr);
            assert(l == -rr && "L/R tear: channels from different frames");
        }
        ring.commitRead(r + (uint64_t) pull);
        checked += (uint64_t) pull;
    }
    stop.store(true);
    producer.join();
    std::printf("packed_stereo_ring: tear-check ok (%llu frames, %llu laps)\n",
                (unsigned long long) checked, (unsigned long long) laps.load());
    assert(laps.load() > 0 && "stress never lapped — laps path untested, tune sizes");
}

// The split output path pulls min(outSamples, avail) into scratch but
// consumes min(numSamples, avail) so a scratch-clamped block doesn't
// accumulate ring/output-clock skew. Pin that index arithmetic.
static void testPullVsConsumeSkew()
{
    PackedStereoRing<64> ring;
    float L[40], R[40];
    for (int i = 0; i < 40; ++i) { L[i] = (float) i; R[i] = 0.0f; }
    ring.push(L, R, 40);

    const int numSamples = 40; // device block
    const int outSamples = 32; // scratch-clamped
    uint64_t r = ring.readIndex.load();
    const uint64_t w = ring.writeIndex.load();
    const uint64_t avail = w - r;
    const int pull    = (int) (avail < (uint64_t) outSamples ? avail : (uint64_t) outSamples);
    const int consume = (int) (avail < (uint64_t) numSamples ? avail : (uint64_t) numSamples);
    assert(pull == 32 && consume == 40);
    ring.commitRead(r + (uint64_t) consume);
    assert(ring.writeIndex.load() - ring.readIndex.load() == 0); // no skew left queued
}

int main()
{
    testPackRoundTrip();
    testPushPullBasic();
    testLapCatchUp();
    testResyncAfterReset();
    testPullVsConsumeSkew();
    testConcurrentTearFreedom();
    std::puts("packed_stereo_ring: all cases passed");
    return 0;
}
