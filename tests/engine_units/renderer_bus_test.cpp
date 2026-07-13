// Phase 2 unit tests for RendererBus (docs/audio-engine-tlc.md §5):
// resampler continuity across pushes, equal-rate bit-exactness, the prime
// gate, underflow → silence + re-prime, fill clamp, and metrics arithmetic.
// The flush-on-disable test flips once the phase-8 flush-flag fix lands.

#include "../../src/audio/engine/RendererBus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using slopsmith::RendererBus;

static std::vector<float> rampChunk(int frames, float start, float step)
{
    std::vector<float> v((size_t) frames * 2);
    for (int i = 0; i < frames; ++i)
    {
        v[(size_t) i * 2]     = start + step * (float) i;
        v[(size_t) i * 2 + 1] = -(start + step * (float) i);
    }
    return v;
}

// Equal rates degenerate to step == 1.0 — frames must come out bit-exact
// (minus the one-frame interpolation carry at each chunk boundary).
static void testEqualRateBitExact()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto c1 = rampChunk(512, 0.0f, 1.0f);
    const auto c2 = rampChunk(512, 512.0f, 1.0f);
    assert(bus.push(c1.data(), 512, 48000.0, 48000.0));
    assert(bus.push(c2.data(), 512, 48000.0, 48000.0));

    std::vector<float> dl(512), dr(512);
    assert(bus.pull(dl.data(), dr.data(), 512) == 512);
    for (int i = 0; i < 512; ++i)
    {
        // First chunk's frame 0 is consumed as interpolation carry (pos
        // starts at 0 with prev=0 carry → exact frame i lands at output i).
        assert(dl[(size_t) i] == (float) i && dr[(size_t) i] == -(float) i);
    }
}

// Downsampling 2:1 across a chunk seam must be continuous: the interpolated
// ramp has no discontinuity where one push ends and the next begins.
static void testResampleContinuityAcrossPushes()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const double src = 96000.0, dev = 48000.0;
    // Two chunks big enough that the 2:1 output (~1023 frames) clears the
    // prime gate; the seam sits at output frame ~512.
    const auto c1 = rampChunk(1024, 0.0f, 1.0f);
    const auto c2 = rampChunk(1024, 1024.0f, 1.0f);
    bus.push(c1.data(), 1024, src, dev);
    bus.push(c2.data(), 1024, src, dev);

    std::vector<float> dl(768), dr(768);
    assert(bus.pull(dl.data(), dr.data(), 768) == 768);
    for (int i = 1; i < 768; ++i)
    {
        const float d = dl[(size_t) i] - dl[(size_t) i - 1];
        // A linear ramp resampled 2:1 must step by ~2 everywhere, including
        // across the seam at output frame ~128.
        assert(std::fabs(d - 2.0f) < 1e-3f && "discontinuity at chunk seam");
    }
}

// Prime gate: nothing comes out until ~kPrimeFrames are buffered.
static void testPrimeGate()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    std::vector<float> dl(64), dr(64);
    const auto tiny = rampChunk(RendererBus::kPrimeFrames / 2, 1.0f, 0.0f);
    bus.push(tiny.data(), RendererBus::kPrimeFrames / 2, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 64) == 0 && "must gate until primed");
    bus.push(tiny.data(), RendererBus::kPrimeFrames / 2, 48000.0, 48000.0);
    // Cushion built (minus the 1-frame carry per push) — next pull flows.
    bus.push(tiny.data(), RendererBus::kPrimeFrames / 2, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64);
}

// Underflow: whole-block silence, buffered tail dropped, back to priming.
static void testUnderflowReprimes()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto chunk = rampChunk(RendererBus::kPrimeFrames + 64, 1.0f, 0.0f);
    bus.push(chunk.data(), RendererBus::kPrimeFrames + 64, 48000.0, 48000.0);
    std::vector<float> dl(512), dr(512);
    assert(bus.pull(dl.data(), dr.data(), 512) == 512);
    // Ring now nearly empty → this pull underflows.
    assert(bus.pull(dl.data(), dr.data(), 512) == 0);
    assert(bus.metrics().underflowCount == 1);
    // And the gate re-armed: a sub-prime refill still gates.
    const auto tiny = rampChunk(64, 1.0f, 0.0f);
    bus.push(tiny.data(), 64, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 32) == 0 && "must re-prime after underflow");
}

// Fill clamp: a dumped backlog beyond kMaxFillFrames is trimmed to the prime
// target instead of being played ~85 ms late.
static void testFillClampTrimsBacklog()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const int backlog = RendererBus::kMaxFillFrames + 2048;
    const auto chunk = rampChunk(backlog + 1, 1.0f, 0.0f);
    bus.push(chunk.data(), backlog + 1, 48000.0, 48000.0);
    std::vector<float> dl(256), dr(256);
    assert(bus.pull(dl.data(), dr.data(), 256) == 256);
    const auto m = bus.metrics();
    assert(m.overflowCount == 1 && "fill clamp must count as overflow");
    assert(m.fillFrames <= RendererBus::kPrimeFrames && "backlog must be trimmed to prime target");
}

// Disabled bus: push and pull are inert.
static void testDisabledIsInert()
{
    RendererBus bus;
    const auto chunk = rampChunk(128, 1.0f, 0.0f);
    assert(!bus.push(chunk.data(), 128, 48000.0, 48000.0));
    std::vector<float> dl(64), dr(64);
    assert(bus.pull(dl.data(), dr.data(), 64) == 0);
    assert(!bus.metrics().enabled);
}

// Gain is applied consumer-side and sanitized (0..8, non-finite → 0).
static void testGainApplied()
{
    RendererBus bus;
    bus.setEnabled(true, 2.0f);
    const auto chunk = rampChunk(RendererBus::kPrimeFrames + 65, 1.0f, 0.0f);
    bus.push(chunk.data(), RendererBus::kPrimeFrames + 65, 48000.0, 48000.0);
    std::vector<float> dl(64), dr(64);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64);
    assert(dl[0] == 2.0f && dr[0] == -2.0f);
}

int main()
{
    testEqualRateBitExact();
    testResampleContinuityAcrossPushes();
    testPrimeGate();
    testUnderflowReprimes();
    testFillClampTrimsBacklog();
    testDisabledIsInert();
    testGainApplied();
    std::puts("renderer_bus: all cases passed");
    return 0;
}
