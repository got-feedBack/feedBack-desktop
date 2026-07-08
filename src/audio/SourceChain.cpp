#include "SourceChain.h"
#include "AudioSanitize.h"

#include <cmath>

// SourceChain — implementation. Method bodies are moved verbatim from
// AudioEngine (Phase 0 is a pure extraction; no behavioural change), with the
// global `inputFrameRing` / `rawAudioRing` / detectors now this source's own
// members and the engine's `audioRunning` / `currentSampleRate` read through the
// references bound at construction.

// ── Lifecycle ───────────────────────────────────────────────────────────────

void SourceChain::prepare(double sr, int blockSize)
{
    // Reset the input rings so a stop→start cycle delivers a clean zero-padded
    // cold-start frame instead of mixing in stale samples from the previous run.
    // The audio thread isn't running yet (device-start hook), so relaxed is fine.
    inputFrameRingWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : inputFrameRing)
        slot.store(0.0f, std::memory_order_relaxed);

    rawAudioRingWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : rawAudioRing)
        slot.store(0.0f, std::memory_order_relaxed);

    // Pre-size the zero-output capture scratch to this device's block size so the
    // audio thread doesn't allocate when we hit that path. For the common
    // output > 0 case this storage stays unused.
    if ((int) inputCaptureScratch.size() < blockSize)
        inputCaptureScratch.assign((size_t) blockSize, 0.0f);

    signalChain.prepare(sr, blockSize);
    pitchDetector.prepare(sr, blockSize);
    mlNoteDetector.prepare(sr, blockSize);
    noteVerifier.prepare(sr, blockSize);
    noiseGate.prepare(sr, blockSize);
    tonePolish.prepare(sr);
}

void SourceChain::releaseResources()
{
    signalChain.releaseResources();
    mlNoteDetector.stop();
    noteVerifier.stop();
    inputFrameRingWriteIndex.store(0, std::memory_order_relaxed);
    rawAudioRingWriteIndex.store(0, std::memory_order_relaxed);
}

// ── Per-block processing (audio thread) ──────────────────────────────────────

void SourceChain::processBlock(const float* const* inputData, int numInputChannels,
                               juce::AudioBuffer<float>& buffer, int effectiveOutputChannels,
                               int numSamples) noexcept
{
    const float inGain = inputGain.load();
    const int selectedCh = selectedInputChannel.load();

    // Copy input with gain, handling channel selection. Track how many output
    // channels we've filled so the "zero extras" pass below can clip to the right
    // range — the broadcast branches fill all of them, the pass-through branch
    // only fills the overlap.
    int filledOutputChannels = 0;
    if (selectedCh >= 0 && selectedCh < numInputChannels)
    {
        // Explicit single-channel pick (e.g. dry from a Valeton GP-5 left
        // channel, or a USB guitar cable whose guitar is on a known channel).
        // Broadcast the selected input across all output channels. Works for a
        // mono device too (selectedCh 0 on a 1-channel input).
        for (int outCh = 0; outCh < effectiveOutputChannels; ++outCh)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(outCh, i, inputData[selectedCh][i] * inGain);
        filledOutputChannels = effectiveOutputChannels;
    }
    else if (numInputChannels == 1)
    {
        // Mono input device — the common USB guitar cable enumerates as a single
        // capture channel. Broadcast that one channel across EVERY output channel
        // so the guitar is centred. The old pass-through branch below only filled
        // min(numInputChannels, outputChannels) = 1 channel and zeroed the rest,
        // which put the guitar in the left speaker only on a stereo duplex device
        // (cable-in + speakers-out). This restores mono-in / centred-out.
        for (int outCh = 0; outCh < effectiveOutputChannels; ++outCh)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(outCh, i, inputData[0][i] * inGain);
        filledOutputChannels = effectiveOutputChannels;
    }
    else if (selectedCh < 0 && numInputChannels > 1)
    {
        // Default pair mono mix: average the first two input channels and
        // broadcast to every output channel, so the signal chain, pitch detector,
        // input-frame ring, and the user's monitoring all see the same mono
        // signal. We open all advertised hardware inputs so explicit higher
        // channel picks work, but the default keeps the old first-pair semantics
        // instead of attenuating the signal by averaging every input.
        const int mixChannels = juce::jmin(numInputChannels, 2);
        const float invCh = 1.0f / (float) mixChannels;
        for (int i = 0; i < numSamples; ++i)
        {
            float mix = 0.0f;
            for (int ch = 0; ch < mixChannels; ++ch)
                mix += inputData[ch][i];
            const float gained = mix * invCh * inGain;
            for (int outCh = 0; outCh < effectiveOutputChannels; ++outCh)
                buffer.setSample(outCh, i, gained);
        }
        filledOutputChannels = effectiveOutputChannels;
    }
    else
    {
        // Pass-through: genuine multi-channel in/out with an out-of-range
        // explicit selection, or other configs that map channels 1:1. (The mono
        // and default-pair cases are handled above and always broadcast.)
        const int passThroughChannels = juce::jmin(numInputChannels, effectiveOutputChannels);
        for (int ch = 0; ch < passThroughChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample(ch, i, inputData[ch][i] * inGain);
        filledOutputChannels = passThroughChannels;
    }

    // Zero anything we didn't fill.
    for (int ch = filledOutputChannels; ch < effectiveOutputChannels; ++ch)
        buffer.clear(ch, 0, numSamples);

    // Metering: input level (pre-processing)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < effectiveOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentInputLevel.store(peak);
        float prevPeak = inputPeak.load();
        if (peak > prevPeak) inputPeak.store(peak);
    }

    // Build the mono guitar source. The ML detector and the getInputFrame() ring
    // are fed here, pre-gate (note-detection scoring expects the raw dry signal);
    // the YIN pitch detector is fed lower down, AFTER the noise gate (both the
    // monitored path and the zero-output fallback), so the tuner reads silence as
    // "no pitch" instead of chasing gated noise. Buffer ch 0 holds the post-gain
    // mono signal in both duplex and split paths. Zero-output duplex setups
    // (input-only ASIO/JACK) need the scratch fallback.
    const float* monoSource = nullptr;
    if (effectiveOutputChannels > 0)
    {
        monoSource = buffer.getReadPointer(0);
    }
    else if (numInputChannels > 0 && (int) inputCaptureScratch.size() >= numSamples)
    {
        // Build the mono source mirroring the channel-copy semantics:
        // explicit channel select picks one input; -1 with multi-input averages
        // the first pair; otherwise input channel 0.
        if (selectedCh >= 0 && selectedCh < numInputChannels)
        {
            for (int i = 0; i < numSamples; ++i)
                inputCaptureScratch[(size_t) i] = inputData[selectedCh][i] * inGain;
        }
        else if (selectedCh < 0 && numInputChannels > 1)
        {
            const int mixChannels = juce::jmin(numInputChannels, 2);
            const float invCh = 1.0f / (float) mixChannels;
            for (int i = 0; i < numSamples; ++i)
            {
                float mix = 0.0f;
                for (int ch = 0; ch < mixChannels; ++ch)
                    mix += inputData[ch][i];
                inputCaptureScratch[(size_t) i] = mix * invCh * inGain;
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                inputCaptureScratch[(size_t) i] = inputData[0][i] * inGain;
        }
        monoSource = inputCaptureScratch.data();
    }

    if (monoSource != nullptr)
    {
        // Feed the polyphonic ML detector the dry mono signal (pre-gate).
        // Lock-free and a no-op when ONNX support isn't compiled in.
        mlNoteDetector.pushSamples(monoSource, numSamples);

        // Mirror the same signal into the lock-free ring buffer that backs
        // getInputFrame(). The release-store on the write index pairs with the
        // main-thread reader's acquire load so every sample written below is
        // visible before the index update. Per-slot stores are atomic-relaxed so
        // the concurrent read by getInputFrame() isn't a data race (UB) when the
        // writer laps mid-snapshot.
        const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_relaxed);
        constexpr int kMask = kInputFrameRingCapacity - 1;
        for (int i = 0; i < numSamples; ++i)
            inputFrameRing[(w + (uint64_t) i) & (uint64_t) kMask]
                .store(monoSource[i], std::memory_order_relaxed);
        inputFrameRingWriteIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
    }

    noiseGate.processBlock(buffer);

    // Feed the YIN pitch detector from the POST-gate signal so the tuner reports
    // silence as "no pitch" instead of chasing gated noise.
    if (monoSource != nullptr)
    {
        if (effectiveOutputChannels > 0)
        {
            // monoSource aliases buffer ch0, which the gate just processed in place.
            pitchDetector.pushSamples(monoSource, numSamples);
            // Same post-gate samples feed the tuner's raw-audio ring.
            pushRawAudioFrame(monoSource, numSamples);
        }
        else
        {
            // Zero-output (input-only ASIO/JACK) fallback: buffer has no channel,
            // so the processBlock above was a no-op (NoiseGate early-returns and
            // leaves its envelope untouched when numChannels <= 0). Run the gate
            // once on the scratch mono here — a single real gate pass — so the
            // tuner is gated in this path too. ML / input-frame ring already
            // copied the pre-gate samples above, so gating in place is safe.
            float* scratchPtr = inputCaptureScratch.data();
            juce::AudioBuffer<float> scratchGate(&scratchPtr, 1, numSamples);
            noiseGate.processBlock(scratchGate);
            pitchDetector.pushSamples(scratchPtr, numSamples);
            pushRawAudioFrame(scratchPtr, numSamples);
        }
    }

    // Process through signal chain (VSTs, NAM, IR)
    const bool hasProcessors = signalChain.getNumSlots() > 0;
    juce::MidiBuffer midi;
    signalChain.process(buffer, midi);

    // Contain a divergent chain block before it reaches the IIR/gain/mix and the
    // output: a NAM/IR/VST can emit NaN/Inf or a runaway level (esp. on a live
    // chain rebuild during song load), which otherwise blasts the output and
    // poisons persistent downstream state until an app restart (#403). Scrub here
    // so feed-forward processors self-heal and the failure is a glitch, not a dead
    // engine. Count flagged blocks (relaxed; RT-safe — no logging on the audio
    // thread) for later observability.
    if (hasProcessors)
    {
        int fixed = 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            fixed += slopsmith::sanitizeAudioBlock(buffer.getWritePointer(ch), numSamples);
        if (fixed > 0)
            nonFiniteChainBlocks.fetch_add(1, std::memory_order_relaxed);
    }

    // Monitor mute: silence the guitar pass-through when no processors are loaded.
    // This prevents hearing raw/amp-processed input when the user hasn't set up a
    // chain yet. Backing track still plays through. Suppressed during a song-load
    // chain rebuild so the brief (or failed) empty-chain window doesn't silence
    // the guitar.
    if (monitorMuted.load() && !hasProcessors && !monitorMuteSuppressed.load())
        buffer.clear();

    // Full monitor kill: silence the guitar bus unconditionally — dry AND the
    // processed/amp-sim signal — for users who monitor through their own external
    // rig. Distinct from the dry-only mute above (which a loaded chain bypasses),
    // and NOT subject to the suppression guard. Runs after the chain so the pitch
    // detector / metering still see real signal; the backing track is mixed in
    // later, so it keeps playing.
    if (monitorKill.load())
        buffer.clear();

    // Chain output gain — the amp/tone's output level. Applied to the guitar
    // signal ONLY, before the backing track is mixed in, so switching tone presets
    // changes the guitar level without touching the song volume.
    buffer.applyGain(chainOutputGain.load());

    // Tone Polish — fixed 3-band mastering EQ. Sits on the guitar bus only,
    // between chain output gain and the backing-track mix, so the backing track
    // and master output gain stay bit-untouched. Bypassed at a single atomic load
    // when disabled.
    tonePolish.processBlock(buffer);
}

// ── Ring readers (main thread) ───────────────────────────────────────────────

std::vector<float> SourceChain::getInputFrame(int numSamples) const
{
    if (numSamples <= 0) return {};
    if (numSamples > kInputFrameRingCapacity)
        numSamples = kInputFrameRingCapacity;

    // Acquire pairs with the audio thread's release store of the write index:
    // every sample written into the ring before that index is visible to us here.
    const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_acquire);
    std::vector<float> out((size_t) numSamples, 0.0f);

    // Cold-start: audio thread hasn't filled `numSamples` yet. Return what we
    // have, zero-padded on the *left* so the most-recent samples land at the end
    // of the buffer (the YIN/HPS algorithms expect time-aligned data).
    if (w < (uint64_t) numSamples)
    {
        const size_t available = (size_t) w;
        for (size_t i = 0; i < available; ++i)
            out[(size_t) numSamples - available + i]
                = inputFrameRing[i].load(std::memory_order_relaxed);
        return out;
    }

    constexpr uint64_t kMask = (uint64_t) kInputFrameRingCapacity - 1;
    const uint64_t start = w - (uint64_t) numSamples;
    for (int i = 0; i < numSamples; ++i)
        out[(size_t) i]
            = inputFrameRing[(start + (uint64_t) i) & kMask].load(std::memory_order_relaxed);
    return out;
}

uint64_t SourceChain::getInputSince(uint64_t fromIndex, std::vector<float>& out) const
{
    out.clear();
    // Acquire pairs with the audio thread's release store — every sample written
    // before `w` is visible here.
    const uint64_t w = inputFrameRingWriteIndex.load(std::memory_order_acquire);
    if (fromIndex >= w) return w;  // nothing new

    constexpr uint64_t kCap  = (uint64_t) kInputFrameRingCapacity;
    constexpr uint64_t kMask = kCap - 1;

    // If the caller fell more than a ring behind, the oldest samples were
    // overwritten — start at the oldest still-live sample.
    uint64_t start = fromIndex;
    if (w - start > kCap) start = w - kCap;

    const size_t n = (size_t) (w - start);
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = inputFrameRing[(start + (uint64_t) i) & kMask].load(std::memory_order_relaxed);
    return w;
}

void SourceChain::pushRawAudioFrame(const float* data, int numSamples) noexcept
{
    // Audio thread only. Relaxed per-slot stores (a concurrent reader that laps
    // mid-snapshot would otherwise be a data race); the release store on the
    // write index publishes the samples to getRawAudioFrame()'s acquire load.
    const uint64_t w = rawAudioRingWriteIndex.load(std::memory_order_relaxed);
    constexpr uint64_t kMask = (uint64_t) kRawAudioRingCapacity - 1;
    for (int i = 0; i < numSamples; ++i)
        rawAudioRing[(w + (uint64_t) i) & kMask].store(data[i], std::memory_order_relaxed);
    rawAudioRingWriteIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
}

std::vector<float> SourceChain::getRawAudioFrame(int numSamples) const
{
    if (numSamples <= 0) return {};
    if (numSamples > kRawAudioRingCapacity)
        numSamples = kRawAudioRingCapacity;

    // Acquire pairs with the audio thread's release store of the write index:
    // every sample written into the ring before that index is visible here.
    const uint64_t w = rawAudioRingWriteIndex.load(std::memory_order_acquire);
    std::vector<float> out((size_t) numSamples, 0.0f);

    // Cold-start: audio thread hasn't filled `numSamples` yet. Return what we
    // have, zero-padded on the left so the most-recent samples land at the end (a
    // tuner's pitch algorithm expects time-aligned data).
    if (w < (uint64_t) numSamples)
    {
        const size_t available = (size_t) w;
        for (size_t i = 0; i < available; ++i)
            out[(size_t) numSamples - available + i]
                = rawAudioRing[i].load(std::memory_order_relaxed);
        return out;
    }

    constexpr uint64_t kMask = (uint64_t) kRawAudioRingCapacity - 1;
    const uint64_t start = w - (uint64_t) numSamples;
    for (int i = 0; i < numSamples; ++i)
        out[(size_t) i]
            = rawAudioRing[(start + (uint64_t) i) & kMask].load(std::memory_order_relaxed);
    return out;
}

// ── Detection / scoring (main thread) ────────────────────────────────────────

ChordScorer::Result SourceChain::scoreChord(const ChordScorer::Request& req)
{
    // Fast-path when the device isn't running — the input ring is zeroed in
    // releaseResources() (and stays at zero between init and the first device
    // start), so any FFT here would just produce an all-miss score against a
    // silence buffer. Skip the ring snapshot + FFT and synthesize the same shape.
    if (! audioRunning.load(std::memory_order_relaxed))
    {
        ChordScorer::Result out{};
        out.totalStrings = (int) req.notes.size();
        out.results.reserve(req.notes.size());
        for (const auto& n : req.notes)
        {
            ChordScorer::NoteResult r{};
            r.string = n.string;
            r.fret = n.fret;
            out.results.push_back(r);
        }
        return out;
    }

    // When a Basic Pitch model is loaded, judge the chord against the ML
    // detector's active-pitch set — genuine polyphonic transcription rather than
    // the per-string energy/constraint check. `req.bypassMl` overrides this so the
    // renderer can force the DSP band-energy scorer.
    if (! req.bypassMl && mlNoteDetector.isReady())
        return scoreChordWithMl(req);

    // Snapshot the input ring at the requested window size and forward to the
    // scorer. The renderer never sees audio data — only the result object.
    const int numSamples = (req.numSamples > 0) ? req.numSamples : 4096;
    auto frame = getInputFrame(numSamples);
    // sampleRate is 0 between init() and the first audioDeviceAboutToStart, and
    // can also drop to 0 after a device teardown; floor to 48 kHz so the scorer
    // can still run the FFT against whatever stale audio is in the ring. Mirrors
    // NodeAddon::GetSampleRate's 48 kHz floor.
    double sr = sampleRate.load(std::memory_order_relaxed);
    if (! std::isfinite(sr) || sr <= 0.0) sr = 48000.0;
    return chordScorer.scoreChord(frame.data(), (int) frame.size(), sr, req);
}

namespace
{
juce::String midiNoteName(int midi)
{
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (midi < 0 || midi > 127) return "?";
    return juce::String(names[midi % 12]) + juce::String(midi / 12 - 1);
}
} // namespace

PitchDetector::Detection SourceChain::getActiveDetection() const
{
    // Audio stopped — neither detector has live data. Return no detection rather
    // than letting the YIN fallback surface its last stale note for the whole
    // stopped / cold-start window.
    if (! audioRunning.load(std::memory_order_relaxed))
        return {};

    // Prefer the polyphonic ML detector's dominant pitch when a model is loaded;
    // otherwise fall back to the YIN detector. The shape is identical so
    // getPitchDetection's consumers can't tell which detector answered.
    if (mlNoteDetector.isReady())
    {
        const auto note = mlNoteDetector.getDominantNote();
        PitchDetector::Detection d;
        if (note.midi >= 0)
        {
            d.midiNote = note.midi;
            d.confidence = note.confidence;
            d.frequency = 440.0f * std::pow(2.0f, (float) (note.midi - 69) / 12.0f);
            d.cents = 0.0f;  // ML detection is discrete-pitch — no cents estimate
            d.noteName = midiNoteName(note.midi);
        }
        return d;
    }
    return pitchDetector.getLatestDetection();
}

PitchDetector::Detection SourceChain::getRawPitchDetection() const
{
    // Same stopped/cold-start guard as getActiveDetection() so a halted engine
    // returns no detection rather than a stale note.
    if (! audioRunning.load(std::memory_order_relaxed))
        return {};

    // Always the raw YIN result — never the ML override.
    return pitchDetector.getLatestDetection();
}

ChordScorer::Result SourceChain::scoreChordWithMl(const ChordScorer::Request& req) const
{
    ChordScorer::Result out{};
    out.totalStrings = (int) req.notes.size();
    out.results.reserve(req.notes.size());

    // Standard-tuning MIDI base for this (arrangement, stringCount). nullptr for
    // unsupported pairs — every note then fails closed, mirroring the constraint
    // scorer's fail-closed contract.
    const std::vector<int>* base = ChordScorer::standardMidiFor(req.arrangement, req.stringCount);

    // Mirror ChordScorer's request-shape validation: a tuningOffsets vector whose
    // length doesn't match stringCount is malformed — fail closed.
    const bool validRequest = base != nullptr
        && (int) req.tuningOffsets.size() == req.stringCount;

    // Mirror ChordScorer exactly: a malformed request, or any single out-of-range
    // note, fails the WHOLE chord closed (all-miss).
    bool allValid = validRequest;
    if (allValid)
        for (const auto& n : req.notes)
        {
            if (n.string < 0 || n.string >= req.stringCount || n.fret < 0)
            {
                allValid = false;
                break;
            }
            const int off = req.tuningOffsets[(size_t) n.string];
            // Sum in 64-bit: base/off/capo/fret arrive from IPC as 32-bit ints,
            // so an int sum could overflow before the range check.
            const long long expectedMidi =
                (long long) (*base)[(size_t) n.string] + off + req.capo + n.fret;
            if (expectedMidi < 0 || expectedMidi > 127)
            {
                allValid = false;
                break;
            }
        }

    if (! allValid)
    {
        for (const auto& n : req.notes)
        {
            ChordScorer::NoteResult r{};
            r.string = n.string;
            r.fret = n.fret;
            r.hasCents = false;
            out.results.push_back(r);  // r.hit defaults to false
        }
        out.hitStrings = 0;
        out.score = 0.0f;
        out.isHit = false;
        return out;
    }

    int hits = 0;
    for (const auto& n : req.notes)
    {
        ChordScorer::NoteResult r{};
        r.string = n.string;
        r.fret = n.fret;
        r.hasCents = false;  // ML judges by pitch-class membership, not cents

        if (validRequest && n.string >= 0
            && n.string < (int) base->size() && n.fret >= 0)
        {
            // Expected MIDI exactly as ChordScorer computes it:
            // base + per-string tuning offset + capo + fret.
            const int off = (n.string < (int) req.tuningOffsets.size())
                                ? req.tuningOffsets[(size_t) n.string] : 0;
            const int expectedMidi = (int) (
                (long long) (*base)[(size_t) n.string] + off + req.capo + n.fret);

            float conf = 0.0f;
            bool active = mlNoteDetector.isPitchActive(expectedMidi, &conf);

            // Bend / slide: the sounding pitch is moving — accept a ±2 semitone
            // window around the expected note.
            if (! active && (n.bend || n.slide))
            {
                for (int d = -2; d <= 2 && ! active; ++d)
                    if (d != 0)
                        active = mlNoteDetector.isPitchActive(expectedMidi + d, &conf);
            }
            // Harmonic: the fretted fundamental is suppressed and an overtone
            // sounds — accept the octave or octave+fifth above.
            if (! active && n.harmonic)
                active = mlNoteDetector.isPitchActive(expectedMidi + 12, &conf)
                      || mlNoteDetector.isPitchActive(expectedMidi + 19, &conf);

            r.hit = active;
            r.bandEnergy = conf;  // posteriorgram confidence, 0..1 (energy proxy)
        }

        if (r.hit) ++hits;
        out.results.push_back(r);
    }

    out.hitStrings = hits;
    out.score = out.totalStrings > 0 ? (float) hits / (float) out.totalStrings : 0.0f;
    out.isHit = out.totalStrings > 0 && out.score >= req.minHitRatio;
    return out;
}
