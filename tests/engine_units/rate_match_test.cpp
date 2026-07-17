// Phase 4 unit tests (docs/audio-engine-tlc.md §5): the rate-tolerance and
// midpoint-rounding boundary cases the three previously hand-synced sites in
// AudioEngine.cpp narrated in comments, now pinned against the one shared
// implementation in engine/RateMatch.h.

#include "../../src/audio/engine/RateMatch.h"

#include <cstdio>

using slopsmith::ratesMatch;
using slopsmith::nominalRateCandidate;
using slopsmith::DeviceFormatMismatch;
using slopsmith::validateOpenedDeviceFormat;

namespace {

int g_failed = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (condition)
        return;

    ++g_failed;
    std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", expression, file, line);
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

} // namespace

int main()
{
    // Tolerance is <= 0.5 (not <): a backend reporting 47999.5 against a
    // 48000 nominal sits exactly on the boundary and MUST pass — the probe
    // accepted it, so preflight and post-open verify must too.
    CHECK(ratesMatch(47999.5, 48000.0));
    CHECK(ratesMatch(48000.0, 47999.5));
    CHECK(ratesMatch(48000.0, 48000.0));
    CHECK(!ratesMatch(47999.4, 48000.0));   // 0.6 apart → reject
    CHECK(!ratesMatch(44100.0, 48000.0));

    double c = 0.0;

    // Exact pair → exact nominal.
    CHECK(nominalRateCandidate(48000.0, 48000.0, c) && c == 48000.0);

    // Fractional drift on both sides rounds to the clean nominal.
    CHECK(nominalRateCandidate(47999.5, 48000.0, c) && c == 48000.0);
    CHECK(nominalRateCandidate(48000.4, 48000.1, c) && c == 48000.0);

    // Fail-closed midpoint case from the original comment: 48000.4/48000.6
    // passes the pair check (diff 0.2) but rounds to 48001 (midpoint 48000.5
    // rounds up), which is 0.6 from 48000.4 — outside tolerance of one side,
    // so no candidate is surfaced.
    const bool ok = nominalRateCandidate(48000.4, 48000.6, c);
    CHECK(!ok && "midpoint-rounding must stay fail-closed");

    // Non-matching pair → no candidate at all.
    CHECK(!nominalRateCandidate(44100.0, 48000.0, c));

    // A device setup is successful only when the driver accepted every
    // requested format field. This pins the Helix regression where JUCE
    // returned success for a 512 request while the driver remained at 256.
    CHECK(validateOpenedDeviceFormat(48000.0, 512, 48000.0, 512, true, true)
          == DeviceFormatMismatch::none);
    CHECK(validateOpenedDeviceFormat(48000.0, 512, 44100.0, 512, true, true)
          == DeviceFormatMismatch::sampleRate);
    CHECK(validateOpenedDeviceFormat(48000.0, 512, 48000.0, 256, true, true)
          == DeviceFormatMismatch::bufferSize);
    CHECK(validateOpenedDeviceFormat(48000.0, 512, 48000.0, 512, false, true)
          == DeviceFormatMismatch::inputChannels);
    CHECK(validateOpenedDeviceFormat(48000.0, 512, 48000.0, 512, true, false)
          == DeviceFormatMismatch::outputChannels);

    if (g_failed == 0)
        std::puts("rate_match: all cases passed");

    return g_failed == 0 ? 0 : 1;
}
