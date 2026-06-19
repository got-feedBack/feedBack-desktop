#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

// ── Backing-track loudness normalizer ───────────────────────────────────────
// Brings the SONG's backing track to a target loudness (default -12 LUFS) so
// every song sits at the same level, BEFORE the mixer's backing-volume fader
// (so lowering that fader still lowers it). Short-term BS.1770 K-weighted AGC
// (slow, no pumping) + a brickwall limiter to keep boosted peaks safe.
// RT-safe: no allocation in process(). Standard K-weighting here (full-mix
// music) — unlike the per-tone leveler which is flattened for bass fidelity.
class BackingLeveler
{
public:
    void prepare(double sampleRate)
    {
        sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
        designKWeighting(sr);
        msEnv = 0.0;
        currentGainDb = 0.0;
        limGain = 1.0f;
        for (int ch = 0; ch < 2; ++ch) { kPre[ch].reset(); kRlb[ch].reset(); }
    }

    // Normalize `buf` (first `numSamples`) in place toward `targetLufs`.
    void process(juce::AudioBuffer<float>& buf, int numSamples, float targetLufs)
    {
        const int nc = juce::jmin(2, buf.getNumChannels());
        if (nc <= 0 || numSamples <= 0) return;

        // Short-term (~400 ms) K-weighted mean-square, integrated per sample.
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
        }
        const double lufs = (msEnv > 1e-12) ? (-0.691 + 10.0 * std::log10(msEnv)) : -120.0;
        const bool hasSignal = lufs > -50.0;   // gate: don't lift silence/noise

        double wantedDb = currentGainDb;
        if (hasSignal)
            wantedDb = juce::jlimit(-24.0, 24.0, (double) targetLufs - lufs);

        // Slow gain follower (~300 ms) so it normalizes loudness without pumping.
        const double smCoef = 1.0 - std::exp(-(double) numSamples / (0.300 * sr));
        currentGainDb += (wantedDb - currentGainDb) * juce::jlimit(0.0, 1.0, smCoef);
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
    float limGain = 1.0f;
    Biquad kPre[2], kRlb[2];
};
