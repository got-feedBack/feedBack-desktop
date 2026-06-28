#pragma once
#include "InputRingReader.h"
#include "NoiseGate.h"
#include "TonePolish.h"
#include "SignalChain.h"
#include "PitchDetector.h"
#include "ChordScorer.h"
#include "MlNoteDetector.h"
#include "NoteVerifier.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// SourceChain — one independent capture+detect+monitor chain for a single audio
// input. Owns everything that used to be a singleton on AudioEngine from the mono
// guitar signal downward: the lock-free input rings, the noise gate, the YIN +
// ML pitch detectors, the per-input tone chain (VST/NAM/IR), tone polish, and the
// background NoteVerifier with its own chart + verdict stream. AudioEngine holds a
// vector of these (sources[0] is the legacy default); the audio callback fans the
// device's channels out to each source's processBlock and fans the monitor signals
// back into one output mix.
//
// Phase 0 (this commit) wires exactly one source so behaviour is byte-identical to
// the old single-pipeline engine; multi-source fan-out lands in a later phase.
//
// Thread model is inherited verbatim from AudioEngine: processBlock/prepare/
// releaseResources run on the audio + device-management threads; the ring readers,
// scoreChord, and detection getters run on the N-API/main thread; NoteVerifier owns
// its own background worker. The rings are lock-free SPSC (audio writer / main
// reader). `engineAudioRunning` and `engineSampleRate` are references to the
// owning engine's atomics so the detection guards and sample-rate fallback match
// the engine exactly.
class SourceChain : public InputRingReader
{
public:
    SourceChain(int id,
                const std::atomic<bool>& engineAudioRunning,
                const std::atomic<double>& engineSampleRate)
        : sourceId(id), audioRunning(engineAudioRunning), sampleRate(engineSampleRate),
          noteVerifier(*this) {}

    // Stable id (== pool slot index); handed to JS as the source handle.
    int getId() const { return sourceId; }

    // Whether this chain is a live player. The audio callback skips inactive
    // chains; the engine flips this on addSource/removeSource. Released chains
    // keep their object (pooled) so there is no pointer-reassignment race with
    // the audio thread. sources[0] is active from construction.
    bool isActive() const { return active.load(std::memory_order_acquire); }
    void setActive(bool a) { active.store(a, std::memory_order_release); }

    // ── Lifecycle (audio/device-management thread) ────────────────────────────
    // Reset rings to a clean cold start, size the zero-output scratch, and prepare
    // every DSP unit. Mirrors the per-source half of audioDeviceAboutToStart.
    void prepare(double sr, int blockSize);
    // Release the chain, stop the ML detector + verifier, zero the ring indices.
    // Mirrors the per-source half of audioDeviceStopped.
    void releaseResources();

    // Prepare only the monitor DSP (tone chain + gate + polish) at a sample rate
    // — used by the device-setup paths (applyDuplexSetup / applySplitSetup),
    // which historically prepared just these three before the full prepare() runs
    // from audioDeviceAboutToStart. Kept narrow so behaviour is byte-identical.
    void prepareMonitorChain(double sr, int blockSize)
    {
        signalChain.prepare(sr, blockSize);
        noiseGate.prepare(sr, blockSize);
        tonePolish.prepare(sr);
    }
    void releaseMonitorChain() { signalChain.releaseResources(); }

    // ── Per-block processing (audio thread, RT-safe) ──────────────────────────
    // Build this source's mono signal from `inputData` (channel select / mono
    // mix per selectedInputChannel + inputGain), feed the ML detector + input
    // ring, gate, feed the YIN detector + raw ring, run the tone chain, sanitize,
    // apply monitor mute + chain gain + tone polish — leaving the processed
    // monitor signal in `buffer` (the same in-place semantics as before). For
    // Phase 0 `buffer` is the device output buffer and `effectiveOutputChannels`
    // its channel count.
    void processBlock(const float* const* inputData, int numInputChannels,
                      juce::AudioBuffer<float>& buffer, int effectiveOutputChannels,
                      int numSamples) noexcept;

    // ── Accessors for the AudioEngine facade / NodeAddon ──────────────────────
    SignalChain& getSignalChain() { return signalChain; }
    PitchDetector& getPitchDetector() { return pitchDetector; }
    MlNoteDetector& getMlNoteDetector() { return mlNoteDetector; }
    bool loadNoteModel(const juce::File& modelFile) { return mlNoteDetector.loadModel(modelFile); }
    bool hasMlNoteDetector() const { return mlNoteDetector.isAvailable(); }

    PitchDetector::Detection getActiveDetection() const;
    PitchDetector::Detection getRawPitchDetection() const;
    ChordScorer::Result scoreChord(const ChordScorer::Request& req);

    // Post-gate raw mono snapshot for the external tuner plugin (distinct from
    // the pre-gate getInputFrame ring). Not part of InputRingReader.
    std::vector<float> getRawAudioFrame(int numSamples = 4096) const;

    void setChart(const NoteVerifier::ChartUpdate& chart) { noteVerifier.setChart(chart); }
    void clearChart() { noteVerifier.clearChart(); }
    std::vector<NoteVerifier::Verdict> getNoteVerdicts() { return noteVerifier.drainVerdicts(); }
    void setPlayhead(double songTime, bool playing) { noteVerifier.setPlayhead(songTime, playing); }
    // Per-source capture-latency correction (seconds), applied to the verifier
    // playhead. Two INDEPENDENT components that SUM: the AUTO part is the engine's
    // measured (extra-primary) device input-latency delta; the USER part is the
    // renderer's manual fine-tune. Keeping them separate means a user nudge refines
    // — rather than discards — the hardware compensation on platforms that report
    // latency. 0 on the primary device.
    void setVerifierAutoOffset(double seconds)
    {
        verifierAutoOffset.store(seconds, std::memory_order_relaxed);
        noteVerifier.setPlayheadOffset(seconds + verifierUserOffset.load(std::memory_order_relaxed));
    }
    void setVerifierUserOffset(double seconds)
    {
        verifierUserOffset.store(seconds, std::memory_order_relaxed);
        noteVerifier.setPlayheadOffset(verifierAutoOffset.load(std::memory_order_relaxed) + seconds);
    }

    void setNoiseGate(bool enabled, float thresholdDb, float releaseMs, float depthDb)
    {
        noiseGate.setParameters(enabled, thresholdDb, releaseMs, depthDb);
    }
    void setTonePolishEnabled(bool enabled) { tonePolish.setEnabled(enabled); }

    void setInputGain(float gain) { inputGain.store(gain); }
    float getInputGain() const { return inputGain.load(); }
    void setChainOutputGain(float gain) { chainOutputGain.store(gain); }
    float getChainOutputGain() const { return chainOutputGain.load(); }
    void setInputChannel(int channel) { selectedInputChannel.store(channel); }
    int getInputChannel() const { return selectedInputChannel.load(); }
    // Which physical input device this source captures from. 0 = the primary
    // input device (the legacy single-device path; every source today). Phase 2
    // lets a source bind an ADDITIONAL device's slot so two separate interfaces
    // (e.g. two USB cables) each feed their own sources at their own clock —
    // only that device's callback processes this source. selectedInputChannel is
    // then a channel index WITHIN the bound device.
    void setDeviceKey(int key) { deviceKey.store(key, std::memory_order_release); }
    int getDeviceKey() const { return deviceKey.load(std::memory_order_acquire); }
    void setMonitorMute(bool mute) { monitorMuted.store(mute); }
    bool isMonitorMuted() const { return monitorMuted.load(); }
    void setMonitorMuteSuppressed(bool s) { monitorMuteSuppressed.store(s); }
    bool isMonitorMuteSuppressed() const { return monitorMuteSuppressed.load(); }
    // Full monitor kill — silences the guitar bus UNCONDITIONALLY (dry AND the
    // processed/amp-sim signal), unlike setMonitorMute which only mutes the dry
    // pass-through when no processors are loaded. For users who monitor through
    // their own external rig and want zero in-app monitoring. Default off, so
    // existing amp-sim monitoring is unaffected.
    void setMonitorKill(bool kill) { monitorKill.store(kill); }
    bool isMonitorKilled() const { return monitorKill.load(); }

    float getInputLevel() const { return currentInputLevel.load(); }
    float getInputPeak() const { return inputPeak.load(); }
    void resetInputPeak() { inputPeak.store(0.0f); }
    // Clear instantaneous level + latched peak — used when a pooled chain is reused
    // for a new source so it doesn't report the previous player's meters until fresh
    // audio arrives (getSourceLevels() exposes these per source).
    void resetInputMeters() { currentInputLevel.store(0.0f); inputPeak.store(0.0f); }
    uint32_t getNonFiniteChainBlocks() const { return nonFiniteChainBlocks.load(std::memory_order_relaxed); }

    // ── InputRingReader ───────────────────────────────────────────────────────
    std::vector<float> getInputFrame(int numSamples = 4096) const override;
    uint64_t getInputSince(uint64_t fromIndex, std::vector<float>& out) const override;
    double getCurrentSampleRate() const override { return sampleRate.load(std::memory_order_relaxed); }

private:
    // ML-backed chord scoring against the MlNoteDetector's active-pitch set.
    ChordScorer::Result scoreChordWithMl(const ChordScorer::Request& req) const;
    // Append post-gate mono samples to rawAudioRing (audio-thread only, RT-safe).
    void pushRawAudioFrame(const float* data, int numSamples) noexcept;

    const int sourceId;
    std::atomic<bool> active{false};

    // Engine-owned shared state (read-only here): the run-state guard and the
    // device sample rate, exactly as the original AudioEngine methods consulted.
    const std::atomic<bool>& audioRunning;
    const std::atomic<double>& sampleRate;

    // ── DSP units (moved verbatim from AudioEngine) ───────────────────────────
    SignalChain signalChain;
    PitchDetector pitchDetector;
    MlNoteDetector mlNoteDetector;
    NoiseGate noiseGate;
    TonePolish tonePolish;
    ChordScorer chordScorer;
    NoteVerifier noteVerifier;  // constructed with *this as the InputRingReader

    // ── Per-source controls / metering ────────────────────────────────────────
    std::atomic<float> inputGain{1.0f};
    std::atomic<float> chainOutputGain{1.0f};
    std::atomic<float> currentInputLevel{0.0f};
    std::atomic<float> inputPeak{0.0f};
    std::atomic<int> selectedInputChannel{-1}; // -1 = mono mix
    std::atomic<int> deviceKey{0};             // 0 = primary input device
    std::atomic<double> verifierAutoOffset{0.0}; // engine: device-latency delta
    std::atomic<double> verifierUserOffset{0.0}; // renderer: manual fine-tune
    std::atomic<bool> monitorMuted{true};
    std::atomic<bool> monitorMuteSuppressed{false};
    std::atomic<bool> monitorKill{false};
    std::atomic<uint32_t> nonFiniteChainBlocks{0};

    // ── Lock-free SPSC input rings (see AudioEngine.h for the full rationale) ──
    static constexpr int kInputFrameRingCapacity = 8192;
    static_assert((kInputFrameRingCapacity & (kInputFrameRingCapacity - 1)) == 0,
                  "kInputFrameRingCapacity must be a power of two");
    std::array<std::atomic<float>, kInputFrameRingCapacity> inputFrameRing{};
    std::atomic<uint64_t> inputFrameRingWriteIndex{0};

    static constexpr int kRawAudioRingCapacity = 16384;
    static_assert((kRawAudioRingCapacity & (kRawAudioRingCapacity - 1)) == 0,
                  "kRawAudioRingCapacity must be a power of two");
    std::array<std::atomic<float>, kRawAudioRingCapacity> rawAudioRing{};
    std::atomic<uint64_t> rawAudioRingWriteIndex{0};

    // Zero-output capture scratch, pre-sized in prepare() so the hot loop never
    // allocates (input-only ASIO/JACK configs).
    std::vector<float> inputCaptureScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceChain)
};
