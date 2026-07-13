// Phase 4 unit tests (docs/audio-engine-tlc.md §5): the rate-tolerance and
// midpoint-rounding boundary cases the three previously hand-synced sites in
// AudioEngine.cpp narrated in comments, now pinned against the one shared
// implementation in engine/RateMatch.h.

#include "../../src/audio/engine/RateMatch.h"

#include <cassert>
#include <cstdio>

using slopsmith::ratesMatch;
using slopsmith::nominalRateCandidate;

int main()
{
    // Tolerance is <= 0.5 (not <): a backend reporting 47999.5 against a
    // 48000 nominal sits exactly on the boundary and MUST pass — the probe
    // accepted it, so preflight and post-open verify must too.
    assert(ratesMatch(47999.5, 48000.0));
    assert(ratesMatch(48000.0, 47999.5));
    assert(ratesMatch(48000.0, 48000.0));
    assert(!ratesMatch(47999.4, 48000.0));   // 0.6 apart → reject
    assert(!ratesMatch(44100.0, 48000.0));

    double c = 0.0;

    // Exact pair → exact nominal.
    assert(nominalRateCandidate(48000.0, 48000.0, c) && c == 48000.0);

    // Fractional drift on both sides rounds to the clean nominal.
    assert(nominalRateCandidate(47999.5, 48000.0, c) && c == 48000.0);
    assert(nominalRateCandidate(48000.4, 48000.1, c) && c == 48000.0);

    // Fail-closed midpoint case from the original comment: 48000.4/48000.6
    // passes the pair check (diff 0.2) but rounds to 48001 (midpoint 48000.5
    // rounds up), which is 0.6 from 48000.4 — outside tolerance of one side,
    // so no candidate is surfaced.
    const bool ok = nominalRateCandidate(48000.4, 48000.6, c);
    assert(!ok && "midpoint-rounding must stay fail-closed");

    // Non-matching pair → no candidate at all.
    assert(!nominalRateCandidate(44100.0, 48000.0, c));

    std::puts("rate_match: all cases passed");
    return 0;
}
