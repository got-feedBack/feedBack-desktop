#pragma once
#include "SourceChain.h"
#include "GainSanitize.h"
#include "engine/PackedStereoRing.h"
#include "engine/EngineState.h"
#include "engine/RendererBus.h"
#include "engine/Mixer.h"
#include "engine/StreamSink.h"
#include "engine/BackingPlayer.h"
#include "engine/DeviceSetup.h"
#include "engine/SourcePool.h"
#include "engine/ExtraInputs.h"
#include "BackingLeveler.h"
#include "signalsmith-stretch.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() { return inputDeviceManager; }
    juce::AudioDeviceManager& getInputDeviceManager() { return inputDeviceManager; }
    juce::AudioDeviceManager& getOutputDeviceManager() { return outputDeviceManager; }
    // Per-input DSP now lives on a SourceChain; the engine owns sources[0] (the
    // legacy default input) and forwards the single-source API to it. Multi-source
    // fan-out (sources[1..N]) lands in a later phase; the public surface here is
    // unchanged so NodeAddon and the renderer need no change.
    SignalChain& getSignalChain() { return source0().getSignalChain(); }
    PitchDetector& getPitchDetector() { return source0().getPitchDetector(); }
    MlNoteDetector& getMlNoteDetector() { return source0().getMlNoteDetector(); }

    // Arm/suspend the ML note-detection pipeline across every source's detector.
    // Defaults off; the renderer (note_detect) calls this true only while a
    // consumer actually reads ML notes (native-frame detection / non-verifier
    // fallback) and false otherwise, so the default harmonic-comb verifier path
    // — and the always-on home tuner — never pay for ONNX inference. Main thread.
    void setMlNoteDetectionEnabled(bool e);

    // Load the Basic Pitch ONNX model for the polyphonic ML detector. When a
    // model is loaded, getActiveDetection() / scoreChord() route through it;
    // otherwise they fall back to the YIN PitchDetector / ChordScorer.
    bool loadNoteModel(const juce::File& modelFile) { return source0().loadNoteModel(modelFile); }
    bool hasMlNoteDetector() const { return source0().hasMlNoteDetector(); }

    // Best current single-note detection: the ML detector's dominant pitch
    // when a model is loaded, else the YIN detector's latest result. Shape is
    // identical either way so the getPitchDetection bridge is detector-agnostic.
    PitchDetector::Detection getActiveDetection() const { return source0().getActiveDetection(); }

    // Raw monophonic YIN detection, always — bypasses the ML preference so the
    // continuous frequency (sub-Hz, parabolically interpolated) and real cents
    // survive even when a Basic Pitch model is loaded. Backs the tuner's
    // getRawPitch bridge endpoint; the YIN detector reads the post-noise-gate
    // signal, so this is silent (frequency -1) when the gate is closed.
    PitchDetector::Detection getRawPitchDetection() const { return source0().getRawPitchDetection(); }

    // Device enumeration
    struct DeviceTypeInfo
    {
        juce::String name;
        juce::StringArray inputDevices;
        juce::StringArray outputDevices;
    };
    // Device-config shapes moved to engine/DeviceSetup.h (TLC phase 4);
    // aliased so the AudioEngine::DeviceOptions etc. spelling NodeAddon uses
    // is unchanged.
    using DeviceOptions = slopsmith::DeviceOptions;
    using DeviceConfig = slopsmith::DeviceConfig;
    using DeviceConfigResult = slopsmith::DeviceConfigResult;

    struct DeviceMetrics
    {
        uint64_t inputOverflowCount = 0;
        uint64_t outputUnderflowCount = 0;
        // Counts are in audio frames (stereo pairs), not interleaved-float
        // samples — the ring stores 2 floats per slot but the index math
        // and consumer-facing health metric tick once per frame.
        int outputRingFillFrames = 0;
        int outputRingCapacityFrames = 0;
        bool duplex = true;
    };

    juce::Array<DeviceTypeInfo> getDeviceTypes();

    // Phase 2: input devices the user can bind as an ADDITIONAL engine input —
    // restricted to the PRIMARY input's device type (so a JACK pick can't collide
    // with an ALSA primary), minus the device already open as the primary (that's
    // "Main") and minus monitor/loopback pseudo-inputs. Keeps the per-panel device
    // picker to a compatible, sensible set instead of every capture node.
    using BindableInput = slopsmith::ExtraInputs::Bindable;
    std::vector<BindableInput> getBindableInputDevices();

    juce::Array<double> getSampleRates();
    juce::Array<int> getBufferSizes();
    DeviceOptions probeDeviceOptions(const juce::String& typeName,
                                     const juce::String& inputName,
                                     const juce::String& outputName);
    DeviceOptions probeDeviceOptionsDual(const juce::String& inputTypeName,
                                         const juce::String& inputName,
                                         const juce::String& outputTypeName,
                                         const juce::String& outputName);
    juce::String getCurrentDeviceType();    // = getCurrentInputDeviceType
    juce::String getCurrentInputDeviceType();
    juce::String getCurrentOutputDeviceType();
    juce::String getCurrentInputDevice();
    juce::String getCurrentOutputDevice();
    bool isDuplex() const { return duplexMode.load(std::memory_order_relaxed); }
    double getCurrentSampleRate() const { return currentSampleRate.load(std::memory_order_relaxed); }
    int getCurrentBlockSize() const { return inputBlockSize.load(std::memory_order_relaxed); }
    int getCurrentInputBlockSize() const { return inputBlockSize.load(std::memory_order_relaxed); }
    int getCurrentOutputBlockSize() const { return outputBlockSize.load(std::memory_order_relaxed); }
    DeviceMetrics getDeviceMetrics() const;

    bool setDeviceType(const juce::String& typeName);
    bool setInputDeviceType(const juce::String& typeName) { return setDeviceType(typeName); }
    bool setOutputDeviceType(const juce::String& typeName);
    bool setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                        double sampleRate = 48000.0, int bufferSize = 256);
    DeviceConfigResult setAudioDevices(const DeviceConfig& config);

    // Audio start/stop
    void startAudio();
    void stopAudio();
    bool isAudioRunning() const { return audioRunning.load(std::memory_order_relaxed); }

    // Gain controls. Input + chain-output gain are per-source (sources[0]);
    // output gain is the post-mix master and stays engine-global.
    void setInputGain(float gain) { source0().setInputGain(gain); }
    // Sanitized (see GainSanitize.h): a NaN/Inf master gain from JS would
    // multiply the whole device output to NaN downstream of the per-source
    // scrub — clamp at the store so every caller is covered.
    void setOutputGain(float gain) { outputGain.store(slopsmith::sanitizeMasterGain(gain)); }
    float getInputGain() const { return source0().getInputGain(); }
    float getOutputGain() const { return outputGain.load(); }

    // Chain output gain — the amp/tone's output level, applied to the guitar
    // signal before the backing track is mixed. Distinct from outputGain (the
    // post-mix master) so a tone-preset switch doesn't move the song volume.
    void setChainOutputGain(float gain) { source0().setChainOutputGain(gain); }
    float getChainOutputGain() const { return source0().getChainOutputGain(); }

    // Input channel selection (for multi-channel interfaces like Valeton GP-5)
    // 0=left (dry), 1=right (wet), -1=both (mono mix)
    void setInputChannel(int channel) { source0().setInputChannel(channel); }
    int getInputChannel() const { return source0().getInputChannel(); }

    // Monitor mute — when true, input is still processed (pitch detection, metering)
    // but output is silenced unless there are processors in the signal chain
    void setMonitorMute(bool mute) { source0().setMonitorMute(mute); }
    bool isMonitorMuted() const { return source0().isMonitorMuted(); }

    // Monitor-mute suppression — when true, the monitor mute is temporarily
    // overridden so the dry guitar stays audible even with an empty chain.
    // The renderer sets this around a song-load chain rebuild (clear + reload),
    // so the brief empty-chain window doesn't silence the player's guitar.
    void setMonitorMuteSuppressed(bool suppressed) { source0().setMonitorMuteSuppressed(suppressed); }
    bool isMonitorMuteSuppressed() const { return source0().isMonitorMuteSuppressed(); }
    // Refcounted force-mute overrides (see SourceChain's arbiter comment).
    void acquireMonitorMuteHold() { source0().acquireMonitorMuteHold(); }
    void releaseMonitorMuteHold() { source0().releaseMonitorMuteHold(); }
    int getMonitorMuteHoldCount() const { return source0().getMonitorMuteHoldCount(); }
    int getMonitorMuteSuppressCount() const { return source0().getMonitorMuteSuppressCount(); }

    // Full monitor kill — silences the guitar bus entirely (dry + processed),
    // for monitoring through an external rig. Unlike the per-source mute/gain
    // controls (which delegate to source0()), this is a GLOBAL "play through my
    // own rig" preference, so it's applied to EVERY pooled source — active or
    // not — so additional inputs are silenced too and a later addSource()
    // inherits it (addSource never resets the flag). The fixed pool's pointers
    // are never reassigned, and these are plain atomic stores, so iterating it
    // off the control thread is race-free. Default off; see SourceChain.
    void setMonitorKill(bool kill)
    {
        pool.forEach([kill](SourceChain& s) { s.setMonitorKill(kill); });
    }
    bool isMonitorKilled() const { return source0().isMonitorKilled(); }

    // Number of audio blocks whose signal-chain output had to be scrubbed for
    // non-finite/runaway samples (issue #403). A nonzero value means the chain
    // (NAM/IR/VST) emitted garbage that was contained before it reached the
    // output. Exposed for diagnostics.
    uint32_t getNonFiniteChainBlocks() const { return source0().getNonFiniteChainBlocks(); }

    // Noise gate (post-input-gain, pre FX chain; pitch detector sees ungated signal)
    void setNoiseGate(bool enabled, float thresholdDb, float releaseMs, float depthDb)
    {
        source0().setNoiseGate(enabled, thresholdDb, releaseMs, depthDb);
    }

    // Tone Polish — fixed 3-band mastering EQ (HPF 80 Hz, low shelf -3 dB
    // @ 180 Hz, peak -0.5 dB @ 200 Hz Q=1). Applied on the guitar bus only,
    // between chainOutputGain and the backing-track mix, so the backing
    // track and master output gain stay bit-untouched. Defaults on;
    // renderer exposes a per-preset toggle.
    void setTonePolishEnabled(bool enabled) { source0().setTonePolishEnabled(enabled); }

    // Backing track — transport moved to engine/BackingPlayer (TLC phase 3);
    // the volume fader + level meter stay engine-side (mix policy).
    void setBackingVolume(float vol) { backingVolume.store(slopsmith::sanitizeMasterGain(vol)); }
    bool loadBackingTrack(const juce::File& file)
    {
        currentBackingLevel.store(0.0f);
        return backing.load(file);
    }
    void setBackingPosition(double seconds) { backing.setPosition(seconds); }
    void startBacking() { backing.start(); }
    void stopBacking()
    {
        backing.stop();
        currentBackingLevel.store(0.0f);
    }
    void setBackingSpeed(double speed) { backing.setSpeed(speed); }
    // Non-blocking reads — never acquire the backing lock / block the audio callback
    bool isBackingPlaying() const { return backing.isPlaying(); }
    double getBackingPosition() const { return backing.getPosition(); }
    double getBackingDuration() const { return backing.getDuration(); }

    // Metering (read from any thread — atomic). Input level/peak are per-source
    // (sources[0]); output level/peak are the post-mix master, engine-global.
    float getInputLevel() const { return source0().getInputLevel(); }
    float getOutputLevel() const { return currentOutputLevel.load(); }
    float getInputPeak() const { return source0().getInputPeak(); }
    float getOutputPeak() const { return outputPeak.load(); }
    // Running RMS of the backing-track mix bus after the volume fader, updated
    // each audio block by the audio thread. Safe to call from any thread.
    float getBackingLevel() const { return currentBackingLevel.load(); }
    void resetPeaks();

    // ── Streamer mix output (PR1: one stream bus → one extra output device) ───
    // An ADDITIONAL output device carrying an independent submix (game/backing +
    // the guitar monitor mix) for OBS/Discord capture, separate from the local
    // monitor output. Default off → zero behaviour change. Control-thread only.
    // setStreamOutputDevice returns "" on success or an error string.
    juce::String setStreamOutputDevice(const juce::String& typeName, const juce::String& deviceName);
    void clearStreamOutput();
    bool isStreamOutputActive() const { return streamSink.isActive(); }
    juce::String getStreamOutputDeviceName() const { return streamSink.getDesiredDeviceName(); }
    // Bus content: include the backing/game, include the guitar monitor mix, and a
    // linear output gain. All atomic — safe to set live. Gain is sanitised
    // (finite, clamped 0..8) so a NaN/Inf from JS can never reach the stream ring.
    void setStreamBus(bool includeBacking, bool includeGuitar, float gain) { streamSink.setBus(includeBacking, includeGuitar, gain); }
    void setStreamBusGain(float gain) { streamSink.setBusGain(gain); }

    // ── Renderer-audio bus (Phase 2: WebAudio master → engine output) ─────────
    // The renderer pushes its WebAudio master mix here (via IPC) so song/stem
    // audio stays audible when the output device is exclusive-style and the OS
    // mixer path is silenced. SPSC: producer is the main-process IPC thread,
    // consumer is whichever output callback is live (duplex or split). Default
    // off → zero behaviour change.
    void setRendererBus(bool enabled, float gain) { mixer.defaultBus().setEnabled(enabled, gain); }
    // Interleaved stereo frames at `sourceRate`; linear-resampled to the device
    // rate on the producer thread (fractional position + previous frame carried
    // across calls). Returns false when the bus is disabled or the engine is
    // not running. Drop-oldest on overflow, counted.
    bool pushRendererAudio(const float* interleavedLR, int frames, double sourceRate);
    struct RendererBusMetrics
    {
        uint64_t pushedFrames = 0, consumedFrames = 0, underflowCount = 0, overflowCount = 0;
        int fillFrames = 0, capacityFrames = 0;
        bool enabled = false;
    };
    RendererBusMetrics getRendererBusMetrics() const;

    // ── Mixer channel API (ownership plan §5.1, tiers 1–3). Control-thread
    // calls; the audio thread only ever mixes. Channel #0 is the renderer bus
    // above; these manage the bespoke channels. All bounds/values are
    // sanitized natively (tier-2 rule: no JS-side trust).
    int mixerCreateChannel(const char* label, const char* kind, const char* holder)
    {
        return mixer.createChannel(label, kind, holder, isAudioRunning());
    }
    bool mixerReleaseChannel(int id) { return mixer.releaseChannel(id, isAudioRunning()); }
    bool mixerPushChannel(int id, const float* interleavedLR, int frames, double sourceRate)
    {
        return mixer.pushChannel(id, interleavedLR, frames, sourceRate, getCurrentSampleRate());
    }
    bool mixerSetChannelGain(int id, float gain) { return mixer.setChannelGain(id, gain); }
    bool mixerSetChannelMute(int id, bool mute) { return mixer.setChannelMute(id, mute); }
    bool mixerSetChannelGroup(int id, int group) { return mixer.setChannelGroup(id, group); }
    int mixerListChannels(slopsmith::Mixer::ChannelInfo* out) const { return mixer.listChannels(out); }

    float getStreamSinkLevel() const { return streamSink.getLevel(); }
    uint64_t getStreamUnderflowCount() const { return streamSink.getUnderflowCount(); }
    // Producer overflow (drop-oldest): the consumer fell a full ring behind and
    // frames were skipped. Exposed alongside underflow for stream drift diagnosis.
    uint64_t getStreamOverflowCount() const { return streamSink.getOverflowCount(); }

    // Latency
    double getLatencyMs() const;

    // One owner for every latency term (TLC deep-read §5) — the previous
    // three unreconciled truths were getLatencyMs' static half-capacity ring
    // guess, the verifier's input-latency-delta-only offset, and the renderer
    // bus adding prime+fill+resample that no figure surfaced. All ms.
    struct LatencyBreakdown
    {
        double sampleRate = 0.0;
        bool duplex = true;
        double deviceBufferMs = 0.0;     // input buffer (+ output buffer when split)
        double inputLatencyMs = 0.0;     // driver-reported capture latency
        double outputLatencyMs = 0.0;    // driver-reported playback latency
        double splitRingMs = 0.0;        // MEASURED primary-ring residency (0 in duplex)
        double monitorTotalMs = 0.0;     // guitar in→out: buffers + in/out + splitRing
        // Renderer-bus song-audio delay: measured bus fill (includes the
        // ~10.7 ms prime cushion once flowing). 0 when the bus is off.
        double rendererBusMs = 0.0;
    };
    LatencyBreakdown getLatencyBreakdown() const;

    // Raw input frame snapshot for renderer-side polyphonic chord scoring in
    // notedetect. Backed by sources[0]'s pre-gate input ring; the rings (and the
    // power-of-two capacity constants) now live on SourceChain. Default snapshot
    // size matches notedetect's _ND_MIN_YIN_SAMPLES (4096 samples).
    std::vector<float> getInputFrame(int numSamples = 4096) const { return source0().getInputFrame(numSamples); }

    // Gapless input-ring consumption for the onset detector — consecutive calls
    // consume each sample exactly once. See SourceChain::getInputSince for the
    // full gap/shortfall contract.
    uint64_t getInputSince(uint64_t fromIndex, std::vector<float>& out) const { return source0().getInputSince(fromIndex, out); }

    // Post-noise-gate raw mono audio snapshot for the external tuner plugin
    // (distinct from getInputFrame's pre-gate ring). Backed by sources[0].
    std::vector<float> getRawAudioFrame(int numSamples = 4096) const { return source0().getRawAudioFrame(numSamples); }

    // Score a chord against the latest input-ring samples. The chord context
    // (notes, arrangement, thresholds) comes from the renderer over IPC; audio
    // data stays inside the engine. Same `{score, hitStrings, totalStrings,
    // isHit, results[]}` shape as the JS implementation.
    ChordScorer::Result scoreChord(const ChordScorer::Request& req) { return source0().scoreChord(req); }

    // Continuous engine-side chart verification (notedetect). The renderer
    // pushes the song's note chart once via setChart(); a background
    // NoteVerifier thread scores each note's timing window against the live
    // playhead and input ring, and the renderer drains finalized verdicts
    // via getNoteVerdicts(). This replaces the renderer's per-tick
    // scoreChord IPC loop, which starved during dense passages.
    void setChart(const NoteVerifier::ChartUpdate& chart) { source0().setChart(chart); }
    void clearChart() { source0().clearChart(); }
    std::vector<NoteVerifier::Verdict> getNoteVerdicts() { return source0().getNoteVerdicts(); }

    // Renderer's unified, already-corrected playhead — the verifier scores
    // against this rather than getBackingPosition(), which is frozen for
    // HTML5-routed (sloppak) songs. Pushed each detect tick via getNoteVerdicts.
    void setPlayhead(double songTime, bool playing) { source0().setPlayhead(songTime, playing); }

    // ── Multi-input source management ─────────────────────────────────────────
    // A "source" is one independent input chain (its own arrangement chart, note
    // detection, scoring, tone, and monitor). sources[0] always exists. Adding a
    // source binds it to an input channel of the current device (multi-channel
    // interface); separate-device binding lands in a later phase.
    struct SourceInfo
    {
        int id = -1;
        int inputChannel = -1;   // -1 = mono mix of first pair
        int deviceKey = 0;       // 0 = primary input device
        bool active = false;
    };

    // Activate a pooled chain bound to `inputChannel` of input device `deviceKey`
    // (0 = primary device) and return its id, or -1 if the pool is full. Prepares
    // the chain immediately when audio is running so it starts scoring without a
    // device restart. Control-thread only.
    int addSource(int inputChannel, int deviceKey = 0);
    // Deactivate + release a source (id != 0; sources[0] is permanent). Stops its
    // verifier/ML threads; the pooled object is reused by a later addSource.
    bool removeSource(int id);
    // Snapshot of every active source. Control-thread only.
    std::vector<SourceInfo> listSources() const;

    // Phase 2 (multi-device): open `deviceName` as an ADDITIONAL physical input
    // device bound to `deviceKey` (1..kMaxExtraInputDevices) so sources created
    // with addSource(channel, deviceKey) capture from it at its OWN clock. Forces
    // split mode. Returns "" on success or an error string. unbind stops+releases
    // it. activeExtraInputCount = # bound+running extras. Control-thread only.
    juce::String bindInputDevice(int deviceKey, const juce::String& deviceName);
    bool unbindInputDevice(int deviceKey);
    int activeExtraInputCount() const;

    // Per-source accessors for the NodeAddon source-indexed API. Return nullptr
    // for an out-of-range or inactive id (sources[0] always valid).
    SourceChain* getSource(int id);

private:
    // sources[0] is the legacy default input chain; always present + active.
    SourceChain& source0() { return pool.chain0(); }
    const SourceChain& source0() const { return pool.chain0(); }
    // Input-device callback. In duplex it writes outputData directly; in split
    // it pushes processed stereo into outputRing for OutputCallback.
    void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                          int numInputChannels,
                                          float* const* outputData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // Split-mode only: drains outputRing, mixes backing, writes to device.
    void audioOutputCallback(const float* const* inputData,
                             int numInputChannels,
                             float* const* outputData,
                             int numOutputChannels,
                             int numSamples);
    void audioOutputAboutToStart(juce::AudioIODevice* device);
    void audioOutputStopped();

    class OutputCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit OutputCallback(AudioEngine& e) : engine(e) {}
        void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                              int numInputChannels,
                                              float* const* outputData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            engine.audioOutputCallback(inputData, numInputChannels, outputData, numOutputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override { engine.audioOutputAboutToStart(device); }
        void audioDeviceStopped() override { engine.audioOutputStopped(); }
    private:
        AudioEngine& engine;
    };
    OutputCallback outputCallback{ *this };

    // Probe/apply/teardown moved to engine/DeviceSetup (TLC phase 4);
    // setAudioDevices stays here as the orchestrator.
    void teardownSplitMode();

    // Duplex mode: inputDeviceManager owns both directions, outputDeviceManager idle.
    // Split mode: input-only on inputDeviceManager, output-only on outputDeviceManager
    // with an SPSC ring between them.
    juce::AudioDeviceManager inputDeviceManager;
    juce::AudioDeviceManager outputDeviceManager;

    // Shared run-state atomics (TLC phase 1) — the members below are
    // reference aliases under their historical names so call sites are
    // untouched; extracted units take `state` (EngineState&) directly.
    slopsmith::EngineState state;
    std::atomic<bool>& duplexMode = state.duplexMode;
    // Probe/apply/teardown component (TLC phase 4). Holds references only.
    slopsmith::DeviceSetup deviceSetup{ inputDeviceManager, outputDeviceManager, state };

    // Per-input capture+detect+monitor chains + the add/remove/reclaim
    // lifecycle + per-deviceKey quiescence handshake — moved to
    // engine/SourcePool (TLC phase 5). Constants mirrored for the members
    // that size arrays by them (extraInputs, and NodeAddon range checks).
    static constexpr int kMaxSources = slopsmith::SourcePool::kMaxSources;
    static constexpr int kMaxExtraInputDevices = slopsmith::SourcePool::kMaxExtraInputDevices;
    slopsmith::SourcePool pool{ state };
    // Audio-thread scratch for the multi-source mix on the PRIMARY callback:
    // each active source renders its 2-channel monitor here in turn, then it
    // is summed into the output. Pre-sized in audioDeviceAboutToStart so the
    // hot loop never allocates. (Extra devices carry their own scratch.)
    juce::AudioBuffer<float> sourceMonitorScratch;

    // Master output (post-mix) — engine-global, not per-source.
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> backingVolume{0.8f};
    std::atomic<float> currentOutputLevel{0.0f};
    // Per-block RMS of the backing-track mix bus, written by the audio thread
    // and read on the main/JS thread via getBackingLevel(). Computed after the
    // backing volume fader but before the output-gain master so VU meters reflect
    // the track level independently of the post-mix master volume.
    std::atomic<float> currentBackingLevel{0.0f};
    std::atomic<float> outputPeak{0.0f};

    // Backing track — transport/stretch/leveler moved to engine/BackingPlayer
    // (TLC phase 3). Declared after `state` (bound by reference).
    slopsmith::BackingPlayer backing{state};

    // audioRunning keeps its historical DEVICE-STATE semantics (isAudioRunning
    // compat pin); the intent half is state.userWantsAudio — see EngineState.h.
    std::atomic<bool>& audioRunning = state.deviceRunning;
    std::atomic<double>& currentSampleRate = state.currentSampleRate;
    std::atomic<int>& inputBlockSize = state.inputBlockSize;
    std::atomic<int>& outputBlockSize = state.outputBlockSize;

    // The per-input lock-free SPSC rings (pre-gate getInputFrame ring + post-gate
    // getRawAudioFrame ring), the YIN/ML detectors, and the zero-output capture
    // scratch now live on SourceChain — one set per input source. See
    // SourceChain.h for the full lock-free / power-of-two / cold-start rationale.

    // Split-mode SPSC ring (unused in duplex). Packed-LR single-atomic frames
    // — see engine/PackedStereoRing.h for the tear/lock-free rationale (moved
    // there in TLC phase 1). ~85 ms @ 48 kHz — absorbs clock drift over
    // typical sessions.
    static constexpr int kOutputRingFrames = 4096;
    slopsmith::PackedStereoRing<kOutputRingFrames> outputRing;

    // ── Engine-owned mixer (ownership plan §5). Channel #0 is the renderer
    // bus (permanent default, byte-compatible with the pre-mixer surface);
    // bespoke channels are producer-requested. See engine/Mixer.h.
    slopsmith::Mixer mixer;
    // Shared consumer step for the duplex and split output paths: mix one
    // block from every ready mixer channel into `dest` (stereo, per-channel
    // gain ramped, dest cleared first). Returns numSamples when any channel
    // contributed, 0 otherwise. Single consumer — call exactly once per
    // output block; the caller mixes the pulled block into the device output
    // AND hands it to composeAndPushStreamMix so the streamer submix carries
    // renderer-fed song audio too.
    int pullRendererBus(juce::AudioBuffer<float>& dest, int numSamples);
    // Scratches for the per-block mixer pull: the mixed result + one channel
    // pull scratch. Fixed capacity, sized once in about-to-start next to the
    // stream scratches (same no-realloc rule).
    juce::AudioBuffer<float> rendererBusPullScratch;
    juce::AudioBuffer<float> mixerChannelScratch;

    std::atomic<uint64_t> outputUnderflowCount{0};
    std::atomic<uint64_t> inputOverflowCount{0};

    // Pre-sized to outputBlockSize so the pull loop never allocates.
    std::vector<float> outputPullScratchL;
    std::vector<float> outputPullScratchR;
    juce::AudioBuffer<float> outputBackingBuffer;
    bool outputCallbackRegistered = false;
    // Same guard for the primary INPUT callback (`this`): audioRunning can be
    // cleared by a transient audioDeviceStopped() while the callback stays
    // attached, and an unguarded startAudio() re-add would dispatch it twice
    // per block (double DSP + double ring push → half-speed garbled audio) and
    // leave a live registration behind after stopAudio()'s single remove.
    bool inputCallbackRegistered = false;

    // ── Additional input devices — moved to engine/ExtraInputs.{h,cpp}
    // (TLC phase 5). The split output callback drains extraInputs.slots
    // directly; declared after pool/state (bound by reference).
    slopsmith::ExtraInputs extraInputs{ pool, state, inputDeviceManager };
    using InputDeviceSlot = slopsmith::ExtraInputs::InputDeviceSlot;

    // (mixSourcesForDevice moved to SourcePool::mixForDevice — TLC phase 5.)

    // ── Streamer mix output sink — moved to engine/StreamSink.{h,cpp} (TLC
    // phase 2). Declared after `state` (bound by reference).
    slopsmith::StreamSink streamSink{state};
    // Producer-side guitar monitor-mix snapshot (pre-backing), written by the
    // primary/output callback and handed to streamSink.publish(). Sized in
    // audioDeviceAboutToStart / audioOutputAboutToStart alongside the other scratch.
    juce::AudioBuffer<float> streamGuitarScratch;

    // Clamp a requested stream gain to a finite, sane range so a NaN/Inf (or a
    // wild value) from the JS bridge can never be packed into the stream ring.
    static float sanitizeStreamGain(float g) { return slopsmith::sanitizeStreamGain(g); }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
