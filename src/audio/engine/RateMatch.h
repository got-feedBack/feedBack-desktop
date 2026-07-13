#pragma once

// Pure sample-rate matching math shared by probe, preflight, and post-open
// verify (TLC phase 4, deep-read §7 — previously three hand-synced copies in
// AudioEngine.cpp). JUCE-free so tests/engine_units can pin the boundary
// cases the old sites narrated in comments.

#include <cmath>

namespace slopsmith {

// <= 0.5 (not <): a backend reporting 47999.5 against a 48000 nominal has
// |diff| = 0.5 exactly and must pass at every stage the probe accepted it.
inline bool ratesMatch(double a, double b) noexcept
{
    return std::abs(a - b) <= 0.5;
}

// Given a matching in/out rate pair, the clean nominal the probe surfaces to
// the UI (backends sometimes report fractional near-48000 rates; the raw
// value would fail the apply-side setAudioDeviceSetup, which expects an exact
// supported nominal). Returns false when the rounded midpoint falls outside
// tolerance of either side — a matched pair like 48000.4/48000.6 passes the
// |r-r2| check but round(48000.5)=48000/48001 can sit 0.6 from one side; the
// probe stays fail-closed on those.
inline bool nominalRateCandidate(double r, double r2, double& candidate) noexcept
{
    if (!ratesMatch(r, r2)) return false;
    candidate = std::round((r + r2) * 0.5);
    return ratesMatch(r, candidate) && ratesMatch(r2, candidate);
}

} // namespace slopsmith
