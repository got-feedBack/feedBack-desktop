// Mixer unit tests (docs/audio-ownership-plan.md §5/§8): channel #0 stays
// byte-compatible with the plain RendererBus path, bespoke channel lifecycle
// (create / cap refusal / release-fade / control-side reclaim), per-channel
// gain/mute with click-free ramps, group start gates (§8.13), and the
// list/diagnostics surface.

#include "../../src/audio/engine/Mixer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using slopsmith::Mixer;
using slopsmith::RendererBus;

static std::vector<float> constChunk(int frames, float value)
{
    std::vector<float> v((size_t) frames * 2, value);
    return v;
}

static void testChannelZeroByteCompatible()
{
    // The same push/pull sequence through the mixer's default bus must behave
    // exactly like a standalone RendererBus (equal-rate path is bit-exact).
    Mixer m;
    RendererBus reference;
    reference.setEnabled(true, 1.0f);
    m.defaultBus().setEnabled(true, 1.0f);

    auto chunk = constChunk(1024, 0.25f);
    assert(reference.push(chunk.data(), 1024, 48000.0, 48000.0));
    assert(m.defaultBus().push(chunk.data(), 1024, 48000.0, 48000.0));

    float rl[256], rr[256], ml[256], mr[256], sl[256], sr[256];
    const int nRef = reference.pull(rl, rr, 256);

    // Mixer path: pullMixInto ADDS into a cleared destination.
    std::memset(ml, 0, sizeof(ml));
    std::memset(mr, 0, sizeof(mr));
    const int contributed = m.pullMixInto(ml, mr, 256, sl, sr);

    assert(nRef == 256);
    assert(contributed == 1);
    for (int i = 0; i < 256; ++i)
    {
        assert(rl[i] == ml[i]);
        assert(rr[i] == mr[i]);
    }
    std::puts("ok: channel #0 byte-compatible with RendererBus");
}

static void testCreateReleaseAndCap()
{
    Mixer m;
    // Channel 0 exists at construction.
    assert(m.activeChannelCount() == 1);

    int ids[Mixer::kMaxChannels];
    for (int i = 1; i < Mixer::kMaxChannels; ++i)
    {
        ids[i] = m.createChannel("stems", "plugin", "wc:1#stems", false);
        assert(ids[i] == i);
    }
    // Cap reached (§8.9): refusal, not growth.
    assert(m.createChannel("overflow", "plugin", "wc:1#x", false) == -1);
    assert(m.activeChannelCount() == Mixer::kMaxChannels);

    // Channel 0 is never releasable.
    assert(!m.releaseChannel(0, false));

    // Control-side reclaim (no consumer running): release frees immediately.
    assert(m.releaseChannel(ids[1], false));
    assert(m.activeChannelCount() == Mixer::kMaxChannels - 1);
    const int reused = m.createChannel("metronome", "plugin", "wc:2#metronome", false);
    assert(reused == ids[1]);
    std::puts("ok: create / cap refusal / release / slot reuse");
}

static void testReleaseFadesWithLiveConsumer()
{
    Mixer m;
    const int id = m.createChannel("stems", "plugin", "wc:1#stems", true);
    assert(id > 0);
    auto chunk = constChunk(RendererBus::kPrimeFrames + 512, 0.5f);
    assert(m.pushChannel(id, chunk.data(), RendererBus::kPrimeFrames + 512, 48000.0, 48000.0));

    float dl[256], dr[256], sl[256], sr[256];
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    m.pullMixInto(dl, dr, 256, sl, sr);
    assert(std::fabs(dl[128] - 0.5f) < 1e-4f); // audible pre-release

    // Release with a live consumer: slot drains — the next pull fades to
    // silence (start near the running gain, end at exactly zero) then frees.
    assert(m.releaseChannel(id, true));
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    const int contributed = m.pullMixInto(dl, dr, 256, sl, sr);
    assert(contributed == 1);
    assert(std::fabs(dl[0]) > 0.0f);   // fade starts from the running gain
    assert(std::fabs(dl[255]) < 0.01f); // ...and lands at silence
    assert(m.activeChannelCount() == 1);

    // Slot is Free again — reusable.
    const int reused = m.createChannel("next", "plugin", "wc:1#next", true);
    assert(reused == id);
    std::puts("ok: release fades to silence under a live consumer, then frees");
}

static void testGainMuteRamps()
{
    Mixer m;
    const int id = m.createChannel("sfx", "plugin", "wc:1#sfx", true);
    auto chunk = constChunk(RendererBus::kPrimeFrames * 8, 1.0f);
    m.pushChannel(id, chunk.data(), RendererBus::kPrimeFrames * 8, 48000.0, 48000.0);

    float dl[256], dr[256], sl[256], sr[256];
    // Settle the ramp at unity.
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    m.pullMixInto(dl, dr, 256, sl, sr);
    assert(std::fabs(dl[255] - 1.0f) < 1e-4f);

    // Gain step ramps across the block: start near old, end at new.
    assert(m.setChannelGain(id, 0.5f));
    std::memset(dl, 0, sizeof(dl));
    m.pullMixInto(dl, dr, 256, sl, sr);
    assert(dl[0] > 0.9f);
    assert(std::fabs(dl[255] - 0.5f) < 1e-3f);

    // Mute ramps to zero, unmute ramps back.
    assert(m.setChannelMute(id, true));
    std::memset(dl, 0, sizeof(dl));
    m.pullMixInto(dl, dr, 256, sl, sr);
    assert(std::fabs(dl[255]) < 1e-3f);

    // Native clamp: NaN/huge gains sanitized, never trusted (tier-2 rule).
    assert(m.setChannelGain(id, std::numeric_limits<float>::quiet_NaN()));
    assert(m.setChannelGain(id, 1e9f));
    std::memset(dl, 0, sizeof(dl));
    m.setChannelMute(id, false);
    m.pullMixInto(dl, dr, 256, sl, sr);
    for (int i = 0; i < 256; ++i) assert(std::isfinite(dl[i]) && std::fabs(dl[i]) <= 8.0f);

    // Out-of-range ids refused.
    assert(!m.setChannelGain(99, 1.0f));
    assert(!m.setChannelGain(-1, 1.0f));
    std::puts("ok: gain/mute ramps + native clamp");
}

static void testGroupStartGate()
{
    Mixer m;
    const int a = m.createChannel("stem-a", "plugin", "wc:1#stems", true);
    const int b = m.createChannel("stem-b", "plugin", "wc:1#stems", true);
    assert(m.setChannelGroup(a, 3));
    assert(m.setChannelGroup(b, 3));

    // Only member A has audio: the group gate must hold BOTH back.
    auto chunk = constChunk(RendererBus::kPrimeFrames * 4, 0.5f);
    m.pushChannel(a, chunk.data(), RendererBus::kPrimeFrames * 4, 48000.0, 48000.0);

    float dl[128], dr[128], sl[128], sr[128];
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    int contributed = m.pullMixInto(dl, dr, 128, sl, sr);
    assert(contributed == 0); // A buffers, gate closed

    // B catches up: gate opens, both play in the same block.
    m.pushChannel(b, chunk.data(), RendererBus::kPrimeFrames * 4, 48000.0, 48000.0);
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    contributed = m.pullMixInto(dl, dr, 128, sl, sr);
    assert(contributed == 2);
    // Both contribute 0.5 → sum 1.0 once the ramps settle.
    assert(std::fabs(dl[127] - 1.0f) < 1e-3f);

    // Ungrouped channels are unaffected by a blocked group.
    const int solo = m.createChannel("solo", "plugin", "wc:2#solo", true);
    m.pushChannel(solo, chunk.data(), RendererBus::kPrimeFrames * 4, 48000.0, 48000.0);
    const int c2 = m.createChannel("stalled", "plugin", "wc:1#stems", true);
    m.setChannelGroup(c2, 3); // rejoins group 3 with no audio → gate closes again
    std::memset(dl, 0, sizeof(dl));
    std::memset(dr, 0, sizeof(dr));
    contributed = m.pullMixInto(dl, dr, 128, sl, sr);
    assert(contributed == 1); // solo only
    std::puts("ok: group start gate (§8.13)");
}

static void testListChannels()
{
    Mixer m;
    const int id = m.createChannel("stems", "plugin", "wc:1#stems", false);
    m.setChannelGain(id, 0.7f);
    m.setChannelGroup(id, 2);

    Mixer::ChannelInfo infos[Mixer::kMaxChannels];
    const int count = m.listChannels(infos);
    assert(count == 2);
    assert(infos[0].id == 0);
    assert(std::strcmp(infos[0].label, "renderer-master") == 0);
    assert(std::strcmp(infos[0].kind, "default") == 0);
    assert(infos[1].id == id);
    assert(std::strcmp(infos[1].label, "stems") == 0);
    assert(std::strcmp(infos[1].holder, "wc:1#stems") == 0);
    assert(std::fabs(infos[1].gain - 0.7f) < 1e-5f);
    assert(infos[1].group == 2);
    assert(infos[1].metrics.capacityFrames == RendererBus::kFrames);

    // Oversized label/holder are truncated, never overflowed.
    std::string longLabel(500, 'x');
    const int id2 = m.createChannel(longLabel.c_str(), "plugin", longLabel.c_str(), false);
    const int count2 = m.listChannels(infos);
    assert(count2 == 3);
    assert(std::strlen(infos[2].label) == Mixer::kLabelMax - 1);
    assert(std::strlen(infos[2].holder) == Mixer::kHolderMax - 1);
    (void) id2;
    std::puts("ok: listChannels + bounded strings");
}

int main()
{
    testChannelZeroByteCompatible();
    testCreateReleaseAndCap();
    testReleaseFadesWithLiveConsumer();
    testGainMuteRamps();
    testGroupStartGate();
    testListChannels();
    std::puts("mixer_test: all ok");
    return 0;
}
