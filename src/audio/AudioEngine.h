#pragma once
#include "SourceChain.h"
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
    struct DeviceOptions
    {
        juce::String type;          // legacy alias = inputType
        juce::String inputType;
        juce::String outputType;
        juce::String input;
        juce::String output;
        juce::StringArray inputChannels;
        juce::StringArray outputChannels;
        juce::Array<double> sampleRates;   // intersection when dual-type
        juce::Array<int> bufferSizes;
        bool compatible = true;     // false when types share no usable sample rate
        juce::String error;
    };

    struct DeviceConfig
    {
        juce::String inputType;
        juce::String inputDevice;
        juce::String outputType;
        juce::String outputDevice;
        double sampleRate = 48000.0;
        int bufferSize = 256;
    };
    struct DeviceConfigResult
    {
        bool ok = false;
        juce::String error;
        double sampleRate = 0.0;
        int inputBlockSize = 0;
        int outputBlockSize = 0;
        bool duplex = true;
    };

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
    struct BindableInput { juce::String typeName; juce::String name; };
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
    void setOutputGain(float gain) { outputGain.store(gain); }
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
        for (auto& s : sources)
            if (s) s->setMonitorKill(kill);
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

    // Backing track
    void setBackingVolume(float vol) { backingVolume.store(vol); }
    bool loadBackingTrack(const juce::File& file);
    void setBackingPosition(double seconds);
    void startBacking();
    void stopBacking();
    void setBackingSpeed(double speed);
    // Non-blocking reads — do not acquire backingLock and never block the audio callback
    bool isBackingPlaying() const { return backingPlaying.load(); }
    double getBackingPosition() const { return cachedBackingPosition.load(); }
    double getBackingDuration() const { return cachedBackingDuration.load(); }

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
    bool isStreamOutputActive() const { return streamSink.active.load(std::memory_order_acquire); }
    juce::String getStreamOutputDeviceName() const { return streamSink.desiredDeviceName; }
    // Bus content: include the backing/game, include the guitar monitor mix, and a
    // linear output gain. All atomic — safe to set live. Gain is sanitised
    // (finite, clamped 0..8) so a NaN/Inf from JS can never reach the stream ring.
    void setStreamBus(bool includeBacking, bool includeGuitar, float gain)
    {
        streamBusIncludeBacking.store(includeBacking, std::memory_order_relaxed);
        streamBusIncludeGuitar.store(includeGuitar, std::memory_order_relaxed);
        streamBusGain.store(sanitizeStreamGain(gain), std::memory_order_relaxed);
    }
    void setStreamBusGain(float gain) { streamBusGain.store(sanitizeStreamGain(gain), std::memory_order_relaxed); }
    float getStreamSinkLevel() const { return streamSinkLevel.load(std::memory_order_relaxed); }
    uint64_t getStreamUnderflowCount() const { return streamSink.underflowCount.load(std::memory_order_relaxed); }
    // Producer overflow (drop-oldest): the consumer fell a full ring behind and
    // frames were skipped. Exposed alongside underflow for stream drift diagnosis.
    uint64_t getStreamOverflowCount() const { return streamSink.overflowCount.load(std::memory_order_relaxed); }

    // Latency
    double getLatencyMs() const;

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
    SourceChain& source0() { return *sources[0]; }
    const SourceChain& source0() const { return *sources[0]; }
    // Input-device callback. In duplex it writes outputData directly; in split
    // it pushes processed stereo into outputPendingRing for OutputCallback.
    void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                          int numInputChannels,
                                          float* const* outputData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void stopBackingNoLock(); // caller holds backingLock

    // Renders one block of the backing track into backingBuffer (1x bypass or
    // phase-vocoder stretch), advances backingHeardPositionSec /
    // cachedBackingPosition, and clears backingPlaying at EOF. Returns the
    // number of output frames written (== jmin(numSamples, backingBuffer cap)).
    // Shared by the duplex and split output callbacks so the two paths can't
    // drift. Precondition: caller holds backingLock and has verified
    // backingTransport && backingPlaying.
    int renderBackingBlockLocked(int numSamples);

    // Split-mode only: drains outputPendingRing, mixes backing, writes to device.
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

    juce::String applyDuplexSetup(const juce::String& inputName,
                                  const juce::String& outputName,
                                  double sampleRate,
                                  int bufferSize);
    DeviceConfigResult applySplitSetup(const DeviceConfig& config);
    void teardownSplitMode();

    // Duplex mode: inputDeviceManager owns both directions, outputDeviceManager idle.
    // Split mode: input-only on inputDeviceManager, output-only on outputDeviceManager
    // with an SPSC ring between them.
    juce::AudioDeviceManager inputDeviceManager;
    juce::AudioDeviceManager outputDeviceManager;
    std::atomic<bool> duplexMode{true};

    // Per-input capture+detect+monitor chains. A FIXED pool, all constructed up
    // front, so adding/removing a source never reassigns a pointer the audio
    // thread is reading — addSource/removeSource only flip an atomic `active`
    // flag (and prepare/release the chain). sources[0] is the legacy default,
    // active from construction and bound to the primary input device. The audio
    // callback fans device channels out to each active source and fans their
    // monitor signals into the output mix. SourceChain reads the engine's
    // audioRunning / currentSampleRate atomics through references bound at
    // construction.
    static constexpr int kMaxSources = 8;
    // Max ADDITIONAL input devices (beyond the primary). Declared here — ahead of the
    // members that size arrays by it (e.g. callbacksInFlight) — though the extra-input
    // slot registry that uses it lives further below.
    static constexpr int kMaxExtraInputDevices = 3;
    std::array<std::unique_ptr<SourceChain>, kMaxSources> sources;
    // Serialises addSource/removeSource (control threads only — never the audio
    // thread, which just reads each slot's atomic `active`).
    std::mutex sourcesMutex;
    // Audio-thread scratch for the multi-source mix: each active source renders
    // its 2-channel monitor here in turn, then it is summed into the output.
    // Pre-sized in audioDeviceAboutToStart so the hot loop never allocates.
    juce::AudioBuffer<float> sourceMonitorScratch;
    // Count of device callback bodies currently executing, PER deviceKey (index 0 =
    // primary input, 1..kMaxExtraInputDevices = each extra-input slot). Each device
    // callback increments its own key on entry and decrements at its real exit.
    // removeSource() flips a source inactive (future callbacks snapshot active once
    // and skip it), then waits to observe THIS SOURCE's deviceKey count == 0 — at
    // that instant no callback that could touch this source is inside processBlock,
    // so it is safe to release. Keying per-deviceKey (not a single global counter) is
    // essential: with the primary + extra inputs on independent clocks they are
    // rarely ALL idle at once, so a global check would strand removals during steady
    // multi-device playback. A wedged callback past the bounded wait DEFERS the
    // release via pendingRelease[], reclaimed later when that key's body is quiescent.
    std::array<std::atomic<int>, kMaxExtraInputDevices + 1> callbacksInFlight{};
    // Sources whose release was deferred (handshake timed out). Reclaimed under
    // sourcesMutex by reclaimPendingReleases() at the next add/removeSource and on
    // device stop, once it is safe (audio stopped or no callback in flight).
    std::array<bool, kMaxSources> pendingRelease{};
    // Release any deferred sources that are now safe to reclaim. Caller holds
    // sourcesMutex (or is the device-stop path, where the callback is gone).
    void reclaimPendingReleases();

    juce::AudioFormatManager formatManager;

    // Master output (post-mix) — engine-global, not per-source.
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> backingVolume{0.8f};
    // Per-song loudness normalizer for the backing track (applied in
    // renderBackingBlockLocked, pre-fader). Owned + driven by the audio thread.
    BackingLeveler backingLeveler;
    double backingLevelerSr = 0.0;
    std::atomic<float> currentOutputLevel{0.0f};
    // Per-block RMS of the backing-track mix bus, written by the audio thread
    // and read on the main/JS thread via getBackingLevel(). Computed after the
    // backing volume fader but before the output-gain master so VU meters reflect
    // the track level independently of the post-mix master volume.
    std::atomic<float> currentBackingLevel{0.0f};
    std::atomic<float> outputPeak{0.0f};

    // Backing track
    // Read-ahead worker that fills the transport's buffer off the audio thread
    // (see loadBackingTrack). Declared BEFORE backingTransport so it is destroyed
    // AFTER it — the transport's BufferingAudioSource holds a pointer to this
    // thread and must be torn down before the thread goes away.
    juce::TimeSliceThread backingReadThread { "BackingReadAhead" };
    std::unique_ptr<juce::AudioFormatReaderSource> backingSource;
    std::unique_ptr<juce::AudioTransportSource> backingTransport;
    signalsmith::stretch::SignalsmithStretch<float> backingStretch;
    juce::AudioBuffer<float> backingInputBuffer; // pulled from transport at device rate
    juce::AudioBuffer<float> backingBuffer; // stretch output, mixed into device buffer
    std::atomic<int> backingStretchLatencySamples{0};
    std::atomic<bool> backingPlaying{false};
    std::atomic<double> cachedBackingPosition{0.0};
    std::atomic<double> cachedBackingDuration{0.0};
    // Heard playhead: accumulates the source frames consumed each block, then
    // clamped to backingTransport->getCurrentPosition() so a short read at EOF
    // can't push it past the real source point. cachedBackingPosition is this
    // value minus the stretcher output latency (zero on the 1x bypass path).
    std::atomic<double> backingHeardPositionSec{0.0};
    // Active playback rate. Mutated ONLY by the audio thread (in
    // renderBackingBlockLocked), coupled with the stretcher reset, so a block
    // is never processed at a new rate with stale stretch state.
    std::atomic<double> backingSpeed{1.0};
    // Lock-free speed hand-off: setBackingSpeed (control thread) publishes the
    // requested rate here and raises backingSpeedChangePending; the audio
    // thread adopts it on the next block. Avoids the control thread blocking on
    // backingLock and starving the RT tryLock (which would drop a backing block
    // mid-slider-drag).
    std::atomic<double> backingPendingSpeed{1.0};
    std::atomic<bool> backingSpeedChangePending{false};
    juce::CriticalSection backingLock;

    // Toggled from startAudio()/stopAudio() (main / device-management
    // threads) and read from isAudioRunning() on the JS thread via the
    // audio-bridge dispatch loop. Plain bool would be a data race;
    // relaxed-atomic is well-defined and compiles to a plain MOV.
    std::atomic<bool> audioRunning{false};
    // Sample rate is written from the JUCE device callbacks (audio
    // thread / device-management thread) and read from arbitrary
    // callers including the JS thread via getCurrentSampleRate(),
    // so a plain double would be a C++ data race. std::atomic<double>
    // is well-defined and lock-free on the platforms we ship; the
    // hot reads use relaxed since the consumer just wants the latest
    // observable value, not a synchronization point.
    std::atomic<double> currentSampleRate{48000.0};
    // Split mode allows different input vs output block sizes; the ring absorbs
    // the asymmetry. DSP prepares against input; backing resampler against output.
    std::atomic<int> inputBlockSize{256};
    std::atomic<int> outputBlockSize{256};

    // The per-input lock-free SPSC rings (pre-gate getInputFrame ring + post-gate
    // getRawAudioFrame ring), the YIN/ML detectors, and the zero-output capture
    // scratch now live on SourceChain — one set per input source. See
    // SourceChain.h for the full lock-free / power-of-two / cold-start rationale.

    // Split-mode SPSC ring (unused in duplex). Each slot packs one stereo frame
    // (L+R floats) into a single 64-bit atomic so the consumer reads both
    // channels in one indivisible load — without packing, the producer's two
    // separate atomic stores could interleave with the consumer's two loads
    // during a drop-oldest wrap, surfacing as L_new+R_old (or vice versa)
    // sample tears. ~85 ms @ 48 kHz — absorbs clock drift over typical sessions.
    static constexpr int kOutputRingFrames = 4096;
    std::array<std::atomic<uint64_t>, kOutputRingFrames> outputPendingRing{};
    static_assert((kOutputRingFrames & (kOutputRingFrames - 1)) == 0,
                  "kOutputRingFrames must be a power of two for mask wraparound");
    // RT-thread reads + writes touch these slots, so a lock-based fallback
    // would risk priority inversion + audible dropouts. On the platforms we
    // ship (x86_64 + arm64 across Linux/macOS/Windows) atomic<uint64_t> is
    // always lock-free; this assert turns a regression into a build error
    // instead of a silent latency degradation if a future platform port
    // breaks the assumption.
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "outputPendingRing requires lock-free atomic<uint64_t> for RT safety");
    static_assert(sizeof(float) == 4,
                  "outputPendingRing pack/unpack assumes 32-bit float");

    // Pack/unpack helpers — std::bit_cast (C++20) is constexpr + alias-safe.
    static inline uint64_t packLR(float l, float r) noexcept
    {
        const uint32_t li = std::bit_cast<uint32_t>(l);
        const uint32_t ri = std::bit_cast<uint32_t>(r);
        return (static_cast<uint64_t>(ri) << 32) | static_cast<uint64_t>(li);
    }
    static inline void unpackLR(uint64_t v, float& l, float& r) noexcept
    {
        l = std::bit_cast<float>(static_cast<uint32_t>(v & 0xFFFFFFFFu));
        r = std::bit_cast<float>(static_cast<uint32_t>(v >> 32));
    }

    std::atomic<uint64_t> outputRingWriteIndex{0};
    std::atomic<uint64_t> outputRingReadIndex{0};
    std::atomic<uint64_t> outputUnderflowCount{0};
    std::atomic<uint64_t> inputOverflowCount{0};

    // Pre-sized to outputBlockSize so the pull loop never allocates.
    std::vector<float> outputPullScratchL;
    std::vector<float> outputPullScratchR;
    juce::AudioBuffer<float> outputBackingBuffer;
    bool outputCallbackRegistered = false;

    // ── Phase 2: additional input devices ────────────────────────────────────
    // Each ADDITIONAL physical input device (a 2nd/3rd USB interface, e.g. two
    // separate cables) gets its own AudioDeviceManager + callback running on its
    // OWN hardware clock, packing its sources' mixed monitor into its own SPSC
    // ring. audioOutputCallback drains+sums every active ring (drop-oldest wrap
    // absorbs each device's drift independently — no cross-device resampling, the
    // failure mode that corrupts a software combine). deviceKey 0 = the primary
    // inputDeviceManager above; deviceKeys 1..kMaxExtraInputDevices map to
    // extraInputs[deviceKey-1]. When any extra device is active the engine runs
    // split (the primary also uses its ring) so the output sum is uniform.
    // (kMaxExtraInputDevices is declared up top, near kMaxSources.)

    // Forwards a JUCE device callback to the engine, tagged with the slot index.
    struct InputSlotCallback : juce::AudioIODeviceCallback
    {
        AudioEngine* engine = nullptr;
        int slot = -1;  // index into extraInputs (deviceKey - 1)
        void audioDeviceIOCallbackWithContext(const float* const* inputData, int numInputChannels,
                                              float* const* outputData, int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ignoreUnused(outputData, numOutputChannels);
            if (engine) engine->extraInputCallback(slot, inputData, numInputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* d) override { if (engine) engine->extraInputAboutToStart(slot, d); }
        void audioDeviceStopped() override { if (engine) engine->extraInputStopped(slot); }
    };

    struct InputDeviceSlot
    {
        juce::AudioDeviceManager manager;
        InputSlotCallback callback;
        std::array<std::atomic<uint64_t>, kOutputRingFrames> ring{};
        std::atomic<uint64_t> writeIndex{0};
        std::atomic<uint64_t> readIndex{0};
        std::atomic<uint64_t> overflowCount{0};
        std::atomic<bool> active{false};      // a device is bound + running
        std::atomic<double> sampleRate{48000.0};
        std::atomic<int> blockSize{256};
        // (extra input latency − primary input latency) in seconds — applied to
        // this device's sources' verifiers so their capture aligns with the
        // primary-corrected playhead. Computed when the device starts.
        std::atomic<double> latencyDeltaSec{0.0};
        // Audio-thread scratch — one set per slot since each slot's callback runs
        // on its own thread (can't share the primary's sourceMonitorScratch).
        juce::AudioBuffer<float> fanScratch;       // the 2ch mix target
        juce::AudioBuffer<float> monitorScratch;   // per-source render in the N>1 path
        int deviceKey = 0;                         // deviceKey this slot serves (slot+1)
        // The device the user WANTS bound here — persistent INTENT, distinct from
        // the transient `active` (currently open). Set by bindInputDevice, cleared
        // only by a user unbind. stopAudio()/reconfigure close the device but keep
        // this so startAudio() re-opens it; this is what survives a device change.
        // Mutated + read on the control thread only.
        juce::String desiredDeviceName;
        // Whether the NEXT extraInputStopped() for this slot is a PERMANENT unbind
        // (deactivate its sources) vs a transient close (keep them to resume). An
        // atomic the control thread sets and the device thread reads, so the
        // permanent-vs-transient decision never races on the juce::String above.
        std::atomic<bool> permanentUnbind { false };
    };
    std::array<InputDeviceSlot, kMaxExtraInputDevices> extraInputs;

    // Per-slot callback hooks (audio + device-management threads).
    void extraInputCallback(int slot, const float* const* inputData, int numInputChannels, int numSamples);
    void extraInputAboutToStart(int slot, juce::AudioIODevice* device);
    void extraInputStopped(int slot);
    // Close an extra device but KEEP its desiredDeviceName (transient close for
    // stop/reconfigure); reopenDesiredExtraInputs() restores them after a (re)start.
    bool closeExtraInputDevice(int slot);
    void reopenDesiredExtraInputs();

    // Shared fan-out used by both the primary and each extra device's callback:
    // mix every active source bound to `deviceKey` into `mixBuf` (using the
    // caller-owned `monitorScratch` for the N>1 render so concurrent device
    // threads never share scratch). Returns the active source count for that key.
    int mixSourcesForDevice(int deviceKey, const float* const* inputData, int numInputChannels,
                            juce::AudioBuffer<float>& mixBuf, juce::AudioBuffer<float>& monitorScratch,
                            int effectiveOutputChannels, int numSamples);
    // Pack a stereo block into a packed-uint64 SPSC ring (producer side).
    void packStereoIntoRing(const juce::AudioBuffer<float>& buf, int numSamples,
                            std::array<std::atomic<uint64_t>, kOutputRingFrames>& ring,
                            std::atomic<uint64_t>& writeIndex);

    // ── Streamer mix output sink (PR1) ───────────────────────────────────────
    // A second OUTPUT AudioDeviceManager on its OWN clock that drains a dedicated
    // SPSC ring fed by the main output path's composed stream submix. This mirrors
    // the InputDeviceSlot pattern INVERTED to the output side: the PRODUCER is the
    // primary/output callback (composeAndPushStreamMix), the CONSUMER is this extra
    // output device's callback (streamSinkCallback). Default off → no behaviour change.
    struct StreamSinkCallback : juce::AudioIODeviceCallback
    {
        AudioEngine* engine = nullptr;
        void audioDeviceIOCallbackWithContext(const float* const* inputData, int numInputChannels,
                                              float* const* outputData, int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ignoreUnused(inputData, numInputChannels);
            if (engine) engine->streamSinkCallback(outputData, numOutputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* d) override { if (engine) engine->streamSinkAboutToStart(d); }
        void audioDeviceStopped() override { if (engine) engine->streamSinkStopped(); }
    };
    struct StreamSink
    {
        StreamSinkCallback callback;
        std::array<std::atomic<uint64_t>, kOutputRingFrames> ring{};
        std::atomic<uint64_t> writeIndex{0};
        std::atomic<uint64_t> readIndex{0};
        std::atomic<uint64_t> underflowCount{0};
        std::atomic<uint64_t> overflowCount{0};
        std::atomic<bool> active{false};
        std::atomic<double> sampleRate{48000.0};
        std::atomic<int> blockSize{256};
        std::vector<float> pullScratchL, pullScratchR;  // sized in streamSinkAboutToStart
        bool callbackRegistered = false;
        bool initialised = false;
        // Declared LAST so it DESTRUCTS FIRST (members tear down in reverse
        // declaration order): the manager's dtor closes the device and detaches
        // `callback` while `callback`/`ring` are still alive — no use-after-free
        // even if an explicit teardown path is ever missed. stopAudio() /
        // closeStreamSinkDevice() also tear it down explicitly before this.
        juce::AudioDeviceManager manager;
        // Persistent INTENT (control thread only): the device the user chose.
        // Survives a stop/restart so reopenDesiredStreamSink() can re-open it.
        juce::String desiredTypeName;
        juce::String desiredDeviceName;
    };
    StreamSink streamSink;
    std::atomic<bool>  streamBusIncludeBacking{true};
    std::atomic<bool>  streamBusIncludeGuitar{true};
    std::atomic<float> streamBusGain{1.0f};
    std::atomic<float> streamSinkLevel{0.0f};
    // Producer-side scratch (written by the primary/output callback): the guitar
    // monitor-mix snapshot (pre-backing) and the composed stream submix. Sized in
    // audioDeviceAboutToStart / audioOutputAboutToStart alongside the other scratch.
    juce::AudioBuffer<float> streamGuitarScratch;
    juce::AudioBuffer<float> streamMixScratch;

    // Clamp a requested stream gain to a finite, sane range so a NaN/Inf (or a
    // wild value) from the JS bridge can never be packed into the stream ring.
    static float sanitizeStreamGain(float g) { return std::isfinite(g) ? juce::jlimit(0.0f, 8.0f, g) : 0.0f; }

    void streamSinkCallback(float* const* outputData, int numOutputChannels, int numSamples);
    void streamSinkAboutToStart(juce::AudioIODevice* device);
    void streamSinkStopped();
    void reopenDesiredStreamSink();
    // Detach + close the stream-sink device but KEEP desiredTypeName/Name, so a
    // stopAudio()/startAudio() cycle re-opens it (intent survives, like extra
    // inputs). Also the single teardown used by the dtor and clearStreamOutput().
    void closeStreamSinkDevice();
    // Compose the stream submix from the captured guitar mix + the just-rendered
    // backing block and pack it into the stream ring. Called from both output
    // callbacks after backing render. `backingBuf` may be null (not playing).
    void composeAndPushStreamMix(const juce::AudioBuffer<float>& guitarMix,
                                 const juce::AudioBuffer<float>* backingBuf,
                                 int backingFrames, float backingVol, int numSamples);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
