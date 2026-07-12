#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

// ── Backing-track loudness normalizer ───────────────────────────────────────
// Brings the SONG's backing track to a target loudness (default -12 LUFS) so
// every song sits at the same level, BEFORE the mixer's backing-volume fader
// (so lowering that fader still lowers it).
//
// v2 — PER-SONG TRIM, not a running AGC. The first version tracked SHORT-TERM
// (400 ms) loudness with a ~300 ms gain follower and ±24 dB of authority: that
// re-converges on every musical section, so quiet verses got boosted toward
// -12 and loud choruses pulled down — the song's own macro-dynamics were
// flattened, and loud→quiet transitions left the gain low for a beat ("the
// song suddenly plays quiet, then swells back"). Loudness normalization should
// behave like a per-track gain (Spotify-style), not a compressor.
//
// Design: BS.1770 K-weighted **integrated** loudness accumulated over the
// song (gated below -50 LUFS so silence/noise doesn't dilute it). The make-up
// gain slews toward (target − integrated) FAST while the measurement is young
// (first ~8 s of signal: up to 6 dB/s, inaudible as the song is just starting)
// and then locks down to a barely-moving trim (0.25 dB/s) — verse/chorus
// dynamics pass through untouched. A -1 dBFS brickwall still guards boosted
// peaks. RT-safe: no allocation in process(). Standard K-weighting (full-mix
// music) — unlike the per-tone leveler which is flattened for bass fidelity.
class BackingLeveler
{
public:
    void prepare(double sampleRate)
    {
        sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
        designKWeighting(sr);
        msEnv = 0.0;
        intSum = 0.0;
        intSamples = 0;
        signalSeconds = 0.0;
        currentGainDb = 0.0;
        limGain = 1.0f;
        for (int ch = 0; ch < 2; ++ch) { kPre[ch].reset(); kRlb[ch].reset(); }
    }

    // Normalize `buf` (first `numSamples`) in place toward `targetLufs`.
    void process(juce::AudioBuffer<float>& buf, int numSamples, float targetLufs)
    {
        const int nc = juce::jmin(2, buf.getNumChannels());
        if (nc <= 0 || numSamples <= 0) return;

        // K-weighted mean-square: a short envelope for the signal gate, and a
        // gated INTEGRATED accumulator for the actual measurement.
        const double rmsCoef = 1.0 - std::exp(-1.0 / (0.400 * sr));
        for (int i = 0; i < numSamples; ++i)
        {
            double sq = 0.0;
            for (int ch = 0; ch < nc; ++ch)
            {
                const double w = kRlb[ch].process(kPre[ch].process((double) buf.getReadPointer(ch)[i]));
                sq += w * w;
            }
            sq /= (double) nc;
            msEnv += rmsCoef * (sq - msEnv);
            // Gate the integration on the short-term envelope so leading
            // silence / count-ins / fade tails don't dilute the measurement.
            if (msEnv > 1.0e-5)   // ≈ -50 LUFS
            {
                intSum += sq;
                ++intSamples;
            }
        }

        const bool haveMeasure = intSamples > (juce::int64) (0.5 * sr);   // ≥ 0.5 s of signal
        if (haveMeasure)
        {
            const double intMs = intSum / (double) intSamples;
            const double integratedLufs = -0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, intMs));
            const double wantedDb = juce::jlimit(-12.0, 12.0, (double) targetLufs - integratedLufs);

            // Slew limit instead of a time-constant follower: fast while the
            // song is starting (the measurement is still forming), then locked
            // to a creep so in-song dynamics are never ridden.
            const double blockSec = (double) numSamples / sr;
            signalSeconds += blockSec;
            const double maxDbPerSec = (signalSeconds < 8.0) ? 6.0 : 0.25;
            const double step = juce::jlimit(-maxDbPerSec * blockSec, maxDbPerSec * blockSec,
                                             wantedDb - currentGainDb);
            currentGainDb += step;
        }
        const float g = (float) juce::Decibels::decibelsToGain(currentGainDb);

        // Brickwall limiter (-1 dBFS ceiling): instant attack, ~100 ms release.
        const float ceil = juce::Decibels::decibelsToGain(-1.0f);
        const float relCoef = 1.0f - std::exp(-1.0f / (0.100f * (float) sr));
        for (int i = 0; i < numSamples; ++i)
        {
            float pk = 0.0f;
            for (int ch = 0; ch < nc; ++ch)
                pk = juce::jmax(pk, std::abs(buf.getReadPointer(ch)[i]) * g);
            const float need = (pk > ceil && pk > 0.0f) ? (ceil / pk) : 1.0f;
            if (need < limGain) limGain = need;
            else                limGain += relCoef * (need - limGain);
            const float tot = g * limGain;
            for (int ch = 0; ch < nc; ++ch)
                buf.getWritePointer(ch)[i] *= tot;
        }
    }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void reset() { z1 = z2 = 0; }
        inline double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void designKWeighting(double fs)
    {
        {   // Stage 1 — +4 dB high-shelf (standard BS.1770)
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(juce::MathConstants<double>::pi * f0 / fs);
            const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad b;
            b.b0 = (Vh + Vb * K / Q + K * K) / a0;
            b.b1 = 2.0 * (K * K - Vh) / a0;
            b.b2 = (Vh - Vb * K / Q + K * K) / a0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            kPre[0] = b; kPre[1] = b;
        }
        {   // Stage 2 — RLB high-pass at 38 Hz (standard BS.1770)
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(juce::MathConstants<double>::pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad b;
            b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            kRlb[0] = b; kRlb[1] = b;
        }
    }
    double sr = 48000.0, msEnv = 0.0, currentGainDb = 0.0;
    double intSum = 0.0;
    juce::int64 intSamples = 0;
    double signalSeconds = 0.0;
    float limGain = 1.0f;
    Biquad kPre[2], kRlb[2];
};
