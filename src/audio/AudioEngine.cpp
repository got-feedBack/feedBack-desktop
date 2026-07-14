#include "AudioEngine.h"
#include "AudioSanitize.h"
#include "VSTTrace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// ── Diagnostic instrumentation (tester builds) ────────────────────────────────
// Every counter is a file-static atomic so the RT paths stay allocation-free.
// First-N logging for anomalies (so a storm can't flood the log) + a periodic
// stats heartbeat every ~5 s of processed audio on each callback clock.
namespace audiodiag {
static std::atomic<uint32_t> primaryReentry{0};      // concurrent primary callback bodies seen
static std::atomic<uint32_t> oversizedBlocks{0};     // numSamples > inputBlockSize on primary
static std::atomic<uint32_t> outputOversized{0};     // numSamples > scratch on output callback
static constexpr uint32_t kFirstN = 25;              // per-anomaly log budget
inline bool firstN(std::atomic<uint32_t>& c) { return c.fetch_add(1, std::memory_order_relaxed) < kFirstN; }
}

// Hard ceiling on backing playback speed. This drives input buffer sizing and runtime clamp.
// Transparent full-speed path — skip the stretcher when rate is effectively 1×.

// On Windows, ASIO drivers can crash with access violations.
// We catch C++ exceptions but can't easily catch SEH in functions with dtors.
// The try/catch blocks around device operations are the best we can do
// without restructuring the code into SEH-safe wrapper functions.

AudioEngine::AudioEngine()
{

    // Start the backing read-ahead worker so the transport's BufferingAudioSource
    // always has a live thread to pull decoded audio on. It sleeps while idle and
    // costs nothing until a track is loaded.

    // The source pool (chains, quiescence handshake) lives on SourcePool now.

    // Extra-input slot registry lives on ExtraInputs now.

    auto result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
    if (result.isNotEmpty())
        std::cerr << "[AudioEngine] input init note: " << result.toStdString() << std::endl;

    auto outResult = outputDeviceManager.initialiseWithDefaultDevices(0, 2);
    if (outResult.isNotEmpty())
        std::cerr << "[AudioEngine] output init note: " << outResult.toStdString() << std::endl;

    // Some backends (WASAPI) bind to a default device on init and would
    // hold it exclusive against the duplex codepath. Idle until split mode.
    outputDeviceManager.closeAudioDevice();

    auto& availableTypes = inputDeviceManager.getAvailableDeviceTypes();
    std::cerr << "[AudioEngine] Available device types: " << availableTypes.size() << std::endl;
    for (auto* type : availableTypes)
    {
        type->scanForDevices();
        std::cerr << "[AudioEngine]   " << type->getTypeName().toStdString()
                  << " - inputs: " << type->getDeviceNames(true).size()
                  << ", outputs: " << type->getDeviceNames(false).size() << std::endl;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
        type->scanForDevices();
}

AudioEngine::~AudioEngine()
{
    // Stop every extra input device FIRST so no slot callback can fire into a
    // half-destroyed engine. closeAudioDevice blocks for the callback thread.
    extraInputs.closeAllForShutdown();
    stopAudio();
    stopBacking();
}

// ── Device Enumeration ────────────────────────────────────────────────────────

juce::Array<AudioEngine::DeviceTypeInfo> AudioEngine::getDeviceTypes()
{
    juce::Array<DeviceTypeInfo> types;

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        DeviceTypeInfo info;
        info.name = type->getTypeName();

        // Use already-scanned device names (scanForDevices was called during init)
        info.inputDevices = type->getDeviceNames(true);
        info.outputDevices = type->getDeviceNames(false);

        types.add(std::move(info));
    }

    return types;
}

std::vector<AudioEngine::BindableInput> AudioEngine::getBindableInputDevices()
{
    return extraInputs.listBindable();
}

juce::Array<double> AudioEngine::getSampleRates()
{
    juce::Array<double> rates;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto rate : device->getAvailableSampleRates())
            rates.add(rate);
    }
    return rates;
}

juce::Array<int> AudioEngine::getBufferSizes()
{
    juce::Array<int> sizes;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto size : device->getAvailableBufferSizes())
            sizes.add(size);
    }
    return sizes;
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptions(const juce::String& typeName,
                                                           const juce::String& inputName,
                                                           const juce::String& outputName)
{
    return probeDeviceOptionsDual(typeName, inputName, typeName, outputName);
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptionsDual(const juce::String& inputTypeName,
                                                               const juce::String& inputName,
                                                               const juce::String& outputTypeName,
                                                               const juce::String& outputName)
{
    return deviceSetup.probeDual(inputTypeName, inputName, outputTypeName, outputName);
}

juce::String AudioEngine::getCurrentDeviceType()
{
    return getCurrentInputDeviceType();
}

juce::String AudioEngine::getCurrentInputDeviceType()
{
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentOutputDeviceType()
{
    if (duplexMode.load(std::memory_order_relaxed))
        return getCurrentInputDeviceType();
    if (auto* type = outputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentInputDevice()
{
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        auto setup = inputDeviceManager.getAudioDeviceSetup();
        return setup.inputDeviceName;
    }
    return {};
}

juce::String AudioEngine::getCurrentOutputDevice()
{
    auto& mgr = duplexMode.load(std::memory_order_relaxed)
        ? inputDeviceManager : outputDeviceManager;
    if (mgr.getCurrentAudioDevice() == nullptr) return {};
    return mgr.getAudioDeviceSetup().outputDeviceName;
}

AudioEngine::DeviceMetrics AudioEngine::getDeviceMetrics() const
{
    DeviceMetrics m;
    m.duplex = duplexMode.load(std::memory_order_relaxed);
    m.inputOverflowCount = inputOverflowCount.load(std::memory_order_relaxed);
    m.outputUnderflowCount = outputUnderflowCount.load(std::memory_order_relaxed);
    // The output ring is only used in split mode. Report 0/0 in duplex so
    // consumers don't think there's a live ring buffer to monitor when
    // there isn't one. Capacity reads-as-zero in duplex matches the
    // "no ring activity" semantic — the ring is structurally inert.
    if (! m.duplex)
    {
        m.outputRingCapacityFrames = kOutputRingFrames;
        // Output device can stop while the input keeps writing, leaving
        // (w - r) larger than capacity. Clamp uint64 → int via the
        // capacity ceiling so the consumer-facing field never overflows
        // or goes negative.
        const uint64_t w = outputRing.writeIndex.load(std::memory_order_acquire);
        const uint64_t r = outputRing.readIndex.load(std::memory_order_acquire);
        const uint64_t fill = (w >= r) ? (w - r) : 0;
        m.outputRingFillFrames = (int) std::min(fill, (uint64_t) kOutputRingFrames);
    }
    return m;
}

double AudioEngine::getLatencyMs() const
{
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    if (sr <= 0.0) return 0.0;

    if (duplexMode.load(std::memory_order_relaxed))
    {
        if (auto* device = inputDeviceManager.getCurrentAudioDevice())
        {
            int latencySamples = device->getCurrentBufferSizeSamples()
                               + device->getInputLatencyInSamples()
                               + device->getOutputLatencyInSamples();
            return (latencySamples / sr) * 1000.0;
        }
        return 0.0;
    }

    int totalSamples = 0;
    if (auto* in = inputDeviceManager.getCurrentAudioDevice())
        totalSamples += in->getCurrentBufferSizeSamples() + in->getInputLatencyInSamples();
    if (auto* out = outputDeviceManager.getCurrentAudioDevice())
        totalSamples += out->getCurrentBufferSizeSamples() + out->getOutputLatencyInSamples();

    // Steady-state ring residency ≈ half capacity once both clocks stabilize.
    totalSamples += kOutputRingFrames / 2;

    return (totalSamples / sr) * 1000.0;
}

AudioEngine::LatencyBreakdown AudioEngine::getLatencyBreakdown() const
{
    LatencyBreakdown b;
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    b.sampleRate = sr;
    b.duplex = duplexMode.load(std::memory_order_relaxed);
    if (sr <= 0.0) return b;
    const auto ms = [sr](double samples) { return (samples / sr) * 1000.0; };

    if (b.duplex)
    {
        if (auto* device = inputDeviceManager.getCurrentAudioDevice())
        {
            b.deviceBufferMs = ms(device->getCurrentBufferSizeSamples());
            b.inputLatencyMs = ms(device->getInputLatencyInSamples());
            b.outputLatencyMs = ms(device->getOutputLatencyInSamples());
        }
    }
    else
    {
        if (auto* in = inputDeviceManager.getCurrentAudioDevice())
        {
            b.deviceBufferMs += ms(in->getCurrentBufferSizeSamples());
            b.inputLatencyMs = ms(in->getInputLatencyInSamples());
        }
        if (auto* out = outputDeviceManager.getCurrentAudioDevice())
        {
            b.deviceBufferMs += ms(out->getCurrentBufferSizeSamples());
            b.outputLatencyMs = ms(out->getOutputLatencyInSamples());
        }
        // MEASURED ring residency — getLatencyMs() still reports the static
        // half-capacity estimate for compatibility; this is the live figure
        // getDeviceMetrics already exposes, converted to time.
        const uint64_t w = outputRing.writeIndex.load(std::memory_order_acquire);
        const uint64_t r = outputRing.readIndex.load(std::memory_order_acquire);
        const uint64_t fill = (w >= r) ? (w - r) : 0;
        b.splitRingMs = ms((double) std::min<uint64_t>(fill, (uint64_t) kOutputRingFrames));
    }
    b.monitorTotalMs = b.deviceBufferMs + b.inputLatencyMs + b.outputLatencyMs + b.splitRingMs;

    // Song audio over the renderer bus is delayed by the measured bus fill —
    // a term no previous latency figure surfaced (deep-read 5).
    const auto busMetrics = mixer.defaultBus().metrics();
    if (busMetrics.enabled)
        b.rendererBusMs = ms((double) busMetrics.fillFrames);
    return b;
}

// ── Device Selection ──────────────────────────────────────────────────────────

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
        {
            fprintf(stderr, "[AudioEngine] Input device type already selected: %s\n", typeName.toRawUTF8());
            return true;
        }
    }

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting input device type: %s\n", typeName.toRawUTF8());
                inputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                // Legacy single-type API contract: callers expect both
                // managers to track the same backend so a subsequent
                // duplex configure or probe sees a consistent dual state.
                // setOutputDeviceType() exists for callers that want to
                // diverge the two sides intentionally. Best-effort — if
                // the output side doesn't expose this backend the input
                // change still stands so duplex on the matched backend
                // keeps working.
                if (auto* currentOutputType = outputDeviceManager.getCurrentDeviceTypeObject())
                {
                    if (currentOutputType->getTypeName() != typeName)
                    {
                        for (auto* outType : outputDeviceManager.getAvailableDeviceTypes())
                        {
                            if (outType->getTypeName() == typeName)
                            {
                                try { outputDeviceManager.setCurrentAudioDeviceType(typeName, true); }
                                catch (...) {
                                    fprintf(stderr, "[AudioEngine] setDeviceType: output sync threw (continuing)\n");
                                }
                                break;
                            }
                        }
                    }
                }
                return true;
            } catch (const std::exception& e) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed: %s\n", e.what());
                return false;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed (unknown)\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setOutputDeviceType(const juce::String& typeName)
{
    if (duplexMode.load(std::memory_order_relaxed))
        return setDeviceType(typeName);

    if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
            return true;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting output device type: %s\n", typeName.toRawUTF8());
                outputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                return true;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setOutputDeviceType crashed\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                                  double sampleRate, int bufferSize)
{
    DeviceConfig c;
    c.inputType  = getCurrentInputDeviceType();
    c.outputType = c.inputType;
    c.inputDevice = inputName;
    c.outputDevice = outputName;
    c.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    c.bufferSize = bufferSize > 0 ? bufferSize : 256;
    return setAudioDevices(c).ok;
}

AudioEngine::DeviceConfigResult AudioEngine::setAudioDevices(const DeviceConfig& config)
{
    DeviceConfigResult res;

    fprintf(stderr, "[AudioEngine] setAudioDevices: inType='%s' inDev='%s' outType='%s' outDev='%s' sr=%.0f bs=%d\n",
            config.inputType.toRawUTF8(), config.inputDevice.toRawUTF8(),
            config.outputType.toRawUTF8(), config.outputDevice.toRawUTF8(),
            config.sampleRate, config.bufferSize);

    // Platform-preferred backend when unspecified. Linux prefers ALSA over
    // JACK (jackd may be installed but not running).
    juce::String resolvedInputType = config.inputType;
    if (resolvedInputType.isEmpty())
    {
        if (auto* t = inputDeviceManager.getCurrentDeviceTypeObject())
            resolvedInputType = t->getTypeName();
    }
    if (resolvedInputType.isEmpty())
    {
#if JUCE_LINUX
        const juce::StringArray preferredOrder { "ALSA", "JACK" };
#elif JUCE_MAC
        const juce::StringArray preferredOrder { "CoreAudio" };
#elif JUCE_WINDOWS
        const juce::StringArray preferredOrder { "Windows Audio", "ASIO" };
#else
        const juce::StringArray preferredOrder;
#endif
        const auto& available = inputDeviceManager.getAvailableDeviceTypes();
        for (const auto& want : preferredOrder)
        {
            for (auto* type : available)
            {
                if (type->getTypeName() == want)
                {
                    resolvedInputType = want;
                    break;
                }
            }
            if (resolvedInputType.isNotEmpty()) break;
        }
        if (resolvedInputType.isEmpty() && !available.isEmpty())
            resolvedInputType = available.getFirst()->getTypeName();
    }

    juce::String resolvedOutputType = config.outputType.isEmpty()
        ? resolvedInputType : config.outputType;

    // Stop audio BEFORE mutating device-type or device setup. JUCE can
    // tear down and re-scan devices inside setCurrentAudioDeviceType /
    // setAudioDeviceSetup, and doing that while the audio callback is
    // still attached risks crashes/deadlocks on some backends (ASIO is
    // the usual culprit). stopAudio() detaches both callbacks first;
    // we'll re-start at the end if we were running.
    //
    // Unconditional: audioDeviceStopped() can clear audioRunning during a
    // transient input stop while the output callback intentionally stays
    // registered for JUCE's auto-restart. A reconfigure that races that
    // window would otherwise skip the stopAudio() detach and leave the
    // stale output callback attached. stopAudio() is itself idempotent
    // (R9 fix — removeAudioCallback is a no-op when not registered), so
    // running it unconditionally is safe regardless of audioRunning.
    // Read USER INTENT, not device state (deep-read §3 fix): audioRunning
    // (deviceRunning) is cleared by transient audioDeviceStopped() fires —
    // WASAPI exclusive opens routinely fire one mid-start — so a reconfigure
    // racing that window used to see false and leave the engine configured
    // but stopped ("no audio until Start/Apply is pressed again").
    // userWantsAudio is written only by start/stopAudio, so it answers the
    // question this restart decision actually asks. NOTE: stopAudio() below
    // clears the intent flag, hence the capture BEFORE it.
    const bool wasRunning = state.userWantsAudio.load(std::memory_order_relaxed);

    // stopAudio() closes every extra input device but KEEPS its desiredDeviceName;
    // the startAudio() below re-opens them at the new config (so panels using a
    // second interface survive a device/sample-rate/buffer change automatically).
    stopAudio();

    // setCurrentAudioDeviceType can throw from inside JUCE backends (ASIO
    // is the usual culprit). Catch and propagate as a structured error so
    // the N-API caller doesn't see the exception cross the boundary.
    try
    {
        if (auto* current = inputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != resolvedInputType)
                inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
        else
        {
            inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for input type '" + resolvedInputType + "'";
        return res;
    }

    // Don't resolve empty names to first-device-of-each-type. Pre-PR
    // behavior — and Copilot's fail-closed concern — treat empty names
    // as "OS default" per side. Filling them with inputs[0] / outputs[0]
    // is JUCE-enumeration-order dependent and can pick the wrong device
    // (e.g. an audio interface that isn't the system default). Both
    // applyDuplexSetup and applySplitSetup handle empty names by setting
    // useDefault*Channels=true, letting JUCE select the OS default.
    const juce::String& resolvedInput  = config.inputDevice;
    const juce::String& resolvedOutput = config.outputDevice;

    const bool sameBackendType = (resolvedInputType == resolvedOutputType);
    const bool sameEndpointIntent = sameBackendType
                                    && config.inputDevice == config.outputDevice;

    // Normalize before branching — applyDuplexSetup() only checks `> 0` and
    // would otherwise let Infinity (or NaN slipping past N-API) reach JUCE
    // on the legacy positional setDevice() path. Finite-and-positive is the
    // baseline both modes need.
    const double requestedSampleRate =
        (std::isfinite(config.sampleRate) && config.sampleRate > 0.0)
            ? config.sampleRate
            : 48000.0;
    const int requestedBufferSize = config.bufferSize > 0 ? config.bufferSize : 256;

    // (Extra input devices were closed by the stopAudio() above with their intent
    // kept; startAudio() below re-opens them at the new config — split mode only.)

    // Only attempt the low-latency COMBINED (duplex) device when input and output
    // are the SAME physical endpoint — a true single-clock duplex device. Two
    // DIFFERENT endpoints of the same backend (e.g. a USB guitar cable in + separate
    // speakers out) are independent hardware clocks; forcing them through one duplex
    // device proved unstable across the app lifecycle (no audio until an explicit
    // Apply, then distortion / dropouts / silent-in-song on navigation). Those route
    // through the split path, whose ring buffer bridges the two clocks. Cross-backend
    // pairs split too. (Low-latency for the two-device case is a separate follow-up —
    // it needs the device-lifecycle work: startup restore + reconfigure-on-nav.)
    if (sameEndpointIntent)
    {
        teardownSplitMode();

        const juce::String err = deviceSetup.applyDuplex(resolvedInput, resolvedOutput,
                                                          requestedSampleRate, requestedBufferSize,
                                                          source0());
        if (err.isEmpty())
        {
            duplexMode.store(true, std::memory_order_relaxed);

            if (auto* dev = inputDeviceManager.getCurrentAudioDevice())
            {
                res.sampleRate = dev->getCurrentSampleRate();
                res.inputBlockSize = dev->getCurrentBufferSizeSamples();
                res.outputBlockSize = res.inputBlockSize;
            }
            res.ok = true;
            res.duplex = true;
        }
        else
        {
            // A same-device config that can't open combined is a real error, not a
            // reason to silently fall to split (which would misrepresent the intent).
            res.error = err;
            res.duplex = true;
            return res;
        }
    }

    if (!res.ok)
    {
        DeviceConfig resolved = config;
        resolved.inputType = resolvedInputType;
        resolved.outputType = resolvedOutputType;
        resolved.inputDevice = resolvedInput;
        resolved.outputDevice = resolvedOutput;
        resolved.sampleRate = requestedSampleRate;
        resolved.bufferSize = requestedBufferSize;

        res = deviceSetup.applySplit(resolved, source0(), outputRing,
                                     outputUnderflowCount, inputOverflowCount,
                                     outputCallback, outputCallbackRegistered);
        if (!res.ok)
            return res;
        duplexMode.store(false, std::memory_order_relaxed);
    }

    // startAudio() re-opens the desired extra input devices at the new config
    // (bindInputDevice forces + verifies the new rate). If the engine was not
    // running, they stay closed with their intent kept until the next startAudio().
    if (wasRunning) startAudio();

    return res;
}

void AudioEngine::teardownSplitMode()
{
    deviceSetup.teardownSplit(outputRing, outputCallback, outputCallbackRegistered);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    // Intent flag first — even if the device open below fails or a transient
    // stop races in, "the user wants audio" survives (read by phase 8's
    // setAudioDevices restart fix; see EngineState.h).
    state.userWantsAudio.store(true, std::memory_order_relaxed);
    if (audioRunning.load(std::memory_order_relaxed))
    {
        fprintf(stderr, "[AudioEngine] startAudio: already running\n");
        return;
    }

    // Input first so it has time to prefill the ring before the output
    // callback pulls — otherwise split mode underflows once at start.
    //
    // Same double-registration guard as the output side below: audioRunning is
    // cleared by audioDeviceStopped() on a transient stop (WASAPI exclusive
    // opens routinely fire one mid-start) while this callback stays attached,
    // so an unguarded re-add here registered the INPUT callback twice — every
    // block then ran the DSP twice and pushed into the split ring twice,
    // playing each sample twice (half speed, one octave down, garbled), and
    // stopAudio()'s single removeAudioCallback left a live registration behind
    // that kept the device (exclusive!) open after "stop" and after app close.
    if (!inputCallbackRegistered)
    {
        inputDeviceManager.addAudioCallback(this);
        inputCallbackRegistered = true;
    }
    else
    {
        // DIAG: this is exactly the path that used to double-register the
        // input callback (half-speed garble + device held open). Now skipped —
        // log it so tester logs prove the guard fired.
        fprintf(stderr, "[diag] startAudio: input callback already registered — skipping re-add (guard active)\n");
    }

    if (!duplexMode.load(std::memory_order_relaxed) && !outputCallbackRegistered)
    {
        outputDeviceManager.addAudioCallback(&outputCallback);
        outputCallbackRegistered = true;
    }

    audioRunning.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[AudioEngine] startAudio: duplex=%d input='%s' output='%s'\n",
            (int) duplexMode.load(std::memory_order_relaxed),
            inputDeviceManager.getCurrentAudioDevice()
                ? inputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none",
            outputDeviceManager.getCurrentAudioDevice()
                ? outputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "(duplex)");

    // Restore any extra input devices the user still wants bound (stopAudio closed
    // them but kept the intent). This is what makes a stop/start cycle or a device
    // reconfigure transparently resume multi-input detection.
    extraInputs.reopenDesired();

    // Restore the streamer-mix output device too (same intent-survives-restart
    // pattern). Best-effort — a failure leaves the sink inactive, never blocks.
    streamSink.reopenDesired();
}

void AudioEngine::stopAudio()
{
    state.userWantsAudio.store(false, std::memory_order_relaxed);
    if (slopsmith_vst_trace::isEnabled())
        fprintf(stderr, "[diag] stopAudio: audioRunning=%d inputCbReg=%d outputCbReg=%d\n",
            (int) audioRunning.load(std::memory_order_relaxed),
            (int) inputCallbackRegistered, (int) outputCallbackRegistered);
    // Always attempt to detach both callbacks — removeAudioCallback is
    // idempotent. We don't gate on audioRunning here because that flag can
    // be cleared externally by audioDeviceStopped() (input device
    // hot-unplug); in split mode the output callback may still be registered
    // even after the input side has reported itself stopped, and a guarded
    // stopAudio() would no-op while leaving the output device producing.
    // Output first so it doesn't pull from a stalling ring during detach.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    inputDeviceManager.removeAudioCallback(this);
    inputCallbackRegistered = false;
    // Extra input devices are opened independently of startAudio(); close them here
    // too so a stopped engine never leaves a second interface capturing, feeding
    // detectors, and holding the hardware open in the background. KEEP their
    // desiredDeviceName so the next startAudio() re-opens them (a stop/start cycle,
    // or a device reconfigure, restores extra inputs automatically). No-op when none
    // are bound — the single-device path is unchanged.
    for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
        extraInputs.closeSlot(dk - 1);
    // Tear down the streamer-mix OUTPUT device too — KEEP its desired intent so the
    // next startAudio() reopens it (same pattern as extra inputs). Without this the
    // 2nd output device keeps running and underflowing while the engine is "stopped",
    // and the destructor (which calls stopAudio()) would be the only path that ever
    // closes it — closing the device here also makes that destructor teardown safe.
    streamSink.close();
    audioRunning.store(false, std::memory_order_relaxed);
    currentBackingLevel.store(0.0f);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

void AudioEngine::resetPeaks()
{
    // Input peak is per-source — clear EVERY active source (getSourceLevels() exposes
    // each one's peak), not just source 0, or extra-device peaks latch forever.
    pool.forEachActive([](SourceChain& s) { s.resetInputPeak(); });
    outputPeak.store(0.0f);
}

// ── Multi-input source management (control thread) ───────────────────────────

int AudioEngine::addSource(int inputChannel, int deviceKey)
{
    // Only deviceKey 0 (primary) and 1..kMaxExtraInputDevices are ever serviced by
    // a callback. An out-of-range key (e.g. stale persisted state) would make an
    // "active" source that mixSourcesForDevice never processes — a ghost that never
    // detects or outputs. Fail fast instead.
    if (deviceKey < 0 || deviceKey > kMaxExtraInputDevices)
        return -1;
    // An extra-device source is only ever serviced by that device's callback, so the
    // device must be BOUND — either currently open (active) or DEFERRED (validated +
    // desired while the engine is stopped, to be reopened by startAudio()). Reject
    // only when the slot is neither: a stale persisted deviceKey, or a slot a
    // reconfigure fully unbound. A deferred source is added now and resumes when its
    // device reopens; without this, a panel restored while audio is stopped can never
    // configure its detector even though the device was successfully (deferred-)bound.
    if (deviceKey >= 1)
    {
        if (! extraInputs.resolveForSource(deviceKey).usable)
            return -1;
    }

    // Resolve device readiness/format/latency for the pool (the extra-device
    // registry is engine-owned; the pool takes resolved values).
    bool deviceReady = audioRunning.load(std::memory_order_relaxed);
    double sr = currentSampleRate.load(std::memory_order_relaxed);
    int bs = inputBlockSize.load(std::memory_order_relaxed);
    double latencyDeltaSec = 0.0;
    if (deviceKey >= 1 && deviceKey <= kMaxExtraInputDevices)
    {
        const auto r = extraInputs.resolveForSource(deviceKey);
        deviceReady = r.ready;
        sr = r.sr;
        bs = r.bs;
        latencyDeltaSec = r.latencyDelta;
    }
    return pool.addResolved(inputChannel, deviceKey, deviceReady, sr, bs, latencyDeltaSec);
}

bool AudioEngine::removeSource(int id)
{
    return pool.remove(id);
}

// Lifetime/threading of getSource() + the NodeAddon *Source* methods:
//  - getSource(), add/removeSource(), and the source-indexed methods all run on
//    the single N-API/JS thread (V8-serialised), so a source can't be removed out
//    from under a getSource() caller on that thread.
//  - getSource() returns a pointer into the FIXED pool. releaseResources() (from
//    removeSource or audioDeviceStopped) only stops the chain's threads + resets
//    its rings — it NEVER frees the SourceChain object — so the pointer never
//    dangles for the life of the engine (no use-after-free).
//  - The only cross-thread overlap is a source-indexed method (N-API) running
//    concurrently with audioDeviceStopped's releaseResources() (device thread).
//    That window is IDENTICAL to the pre-existing legacy methods (scoreChord /
//    getNoteVerdicts / getRawAudioFrame, which all operate on source 0's same
//    chain) and is handled the same way: each component's internals are atomic /
//    individually thread-safe, and the rings are lock-free. This change adds no
//    new race class versus the original single-source engine.
SourceChain* AudioEngine::getSource(int id)
{
    return pool.get(id);
}

void AudioEngine::setMlNoteDetectionEnabled(bool e)
{
    // Fan to every source in the fixed pool (not just active ones) so a source
    // activated later inherits the current arm state instead of silently
    // staying dormant. Each MlNoteDetector::setEnabled is a cheap atomic + a
    // cold-state clear on a real transition.
    pool.forEach([e](SourceChain& s) { s.getMlNoteDetector().setEnabled(e); });
}

std::vector<AudioEngine::SourceInfo> AudioEngine::listSources() const
{
    std::vector<SourceInfo> out;
    for (const auto& i : pool.list())
        out.push_back({ i.id, i.inputChannel, i.deviceKey, i.active });
    return out;
}

// setNoiseGate / setTonePolishEnabled are now inline forwarders to sources[0]
// (see AudioEngine.h).

// ── Audio Callback ────────────────────────────────────────────────────────────

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Fires on the input manager — duplex serves output here too; split has
    // audioOutputAboutToStart on the second manager.
    //
    // Also fires when JUCE auto-restarts the input after a transient stop
    // (hot-replug, OS resume). audioDeviceStopped() cleared audioRunning;
    // restore it here so scoreChord() / getActiveDetection() come back
    // online without requiring a manual stopAudio()/startAudio() cycle,
    // and so a subsequent setAudioDevices() sees the engine as running
    // and runs its defensive stopAudio() before mutating device state.
    audioRunning.store(true, std::memory_order_relaxed);
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    if (slopsmith_vst_trace::isEnabled())
        fprintf(stderr, "[diag] audioDeviceAboutToStart: dev='%s' sr=%.0f bs=%d duplex=%d inputCbReg=%d outputCbReg=%d\n",
            device->getName().toRawUTF8(), sr, bs,
            (int) duplexMode.load(std::memory_order_relaxed),
            (int) inputCallbackRegistered, (int) outputCallbackRegistered);
    currentSampleRate.store(sr, std::memory_order_relaxed);
    inputBlockSize.store(bs, std::memory_order_relaxed);
    if (duplexMode.load(std::memory_order_relaxed))
        outputBlockSize.store(bs, std::memory_order_relaxed);

    // Input callback uses outputBackingBuffer as DSP scratch in split mode.
    // Pre-size against input block size so the hot path can't allocate when
    // inputBlockSize > outputBlockSize (audioOutputAboutToStart only sizes
    // against output).
    if (!duplexMode.load(std::memory_order_relaxed)
        && outputBackingBuffer.getNumSamples() < bs)
    {
        outputBackingBuffer.setSize(2, bs, false, false, true);
    }

    // Pre-size the multi-source mix scratch (2ch) so the audio thread never
    // allocates when summing active sources.
    if (sourceMonitorScratch.getNumSamples() < bs)
        sourceMonitorScratch.setSize(2, bs, false, false, true);

    // Producer-side stream-mix scratch (duplex path runs here). Sized to a FIXED
    // capacity equal to the ring and NEVER grown with the block size: in split mode
    // the OUTPUT callback is the producer that uses these buffers while THIS (input)
    // about-to-start can fire on a hotplug/resume — a block-size-driven realloc here
    // would free memory out from under the live producer. A block larger than the
    // ring can't be published anyway and is skipped by the capacity guard in
    // composeAndPushStreamMix(), so a fixed cap is sufficient AND allocates exactly
    // once: it can never realloc under a live producer, for any later block size.
    constexpr int streamScratchCap = (int) kOutputRingFrames;
    if (streamGuitarScratch.getNumSamples() < streamScratchCap) streamGuitarScratch.setSize(2, streamScratchCap, false, false, true);
    streamSink.prepareProducerScratch();
    if (rendererBusPullScratch.getNumSamples() < streamScratchCap) rendererBusPullScratch.setSize(2, streamScratchCap, false, false, true);
    if (mixerChannelScratch.getNumSamples() < streamScratchCap) mixerChannelScratch.setSize(2, streamScratchCap, false, false, true);

    // Prepare each ACTIVE PRIMARY-device source's DSP and reset its rings for a
    // clean cold start. Inactive pooled chains stay unprepared (no threads). EXTRA-
    // device sources (deviceKey > 0) are prepared by their own extraInputAboutToStart
    // with THAT device's format — a primary restart must not clobber them with the
    // primary's sample rate / block size (they run on a different hardware clock).
    pool.forEachActive([&](SourceChain& s) { if (s.getDeviceKey() == 0) s.prepare(sr, bs); });

    // Split mode preps the backing stretcher in audioOutputAboutToStart
    // instead — that callback owns the device the backing audio actually
    // plays on, and pulls from backingTransport at the output device's
    // block size.
    if (duplexMode.load(std::memory_order_relaxed))
        backing.prepare(sr, bs);
}

void AudioEngine::audioDeviceStopped()
{
    // JUCE calls audioDeviceStopped() only AFTER the PRIMARY input device has
    // stopped invoking its IO callback (stop() blocks for the callback thread to
    // finish), so the primary body is quiescent here. We release ONLY deviceKey-0
    // sources: an EXTRA-device source is processed by that device's own callback,
    // which may still be running on its own thread — releasing it here would race.
    // Extra sources are released by extraInputStopped()/unbindInputDevice().
    if (slopsmith_vst_trace::isEnabled())
        fprintf(stderr, "[diag] audioDeviceStopped (audioRunning cleared; callbacks stay attached for JUCE auto-restart)\n");
    audioRunning.store(false, std::memory_order_relaxed);
    // Release each ACTIVE primary-device source's chain and zero its rings;
    // retries deferred removals now that the primary body is quiescent.
    pool.releaseDeviceSources(0, false, false);
    outputRing.resetIndices();
    currentBackingLevel.store(0.0f);

    // Note on split-mode lifecycle: we deliberately do NOT detach the
    // output callback here. JUCE auto-restarts a transiently-stopped input
    // device by re-firing audioDeviceAboutToStart() on its own; that path
    // doesn't re-add the output callback, so detaching would break
    // automatic recovery (output stays silent until a manual reconfigure).
    // While input is down, the guitar/DSP side of the output goes silent
    // (no producer feeding outputRing, so the consumer's underflow
    // branch zero-fills), but the backing track keeps playing — the output
    // callback mixes backingTransport independently of ring state. That's
    // intentional UX: a user unplugging their interface mid-song doesn't
    // lose their place. Real teardown belongs to stopAudio() / teardownSplitMode().
}

void AudioEngine::audioOutputAboutToStart(juce::AudioIODevice* device)
{
    const int bs = device->getCurrentBufferSizeSamples();
    if (slopsmith_vst_trace::isEnabled())
        fprintf(stderr, "[diag] audioOutputAboutToStart: dev='%s' sr=%.0f bs=%d\n",
            device->getName().toRawUTF8(), device->getCurrentSampleRate(), bs);
    outputBlockSize.store(bs, std::memory_order_relaxed);

    if ((int) outputPullScratchL.size() < bs) outputPullScratchL.assign((size_t) bs, 0.0f);
    if ((int) outputPullScratchR.size() < bs) outputPullScratchR.assign((size_t) bs, 0.0f);

    // Producer-side stream-mix scratch (split path composes the stream submix in
    // this output callback). Fixed capacity == the ring, never grown — a block
    // restart on EITHER clock can't realloc these under a live producer, for any
    // block size (oversized blocks are skipped, not buffered). See the matching
    // note in audioDeviceAboutToStart().
    constexpr int streamScratchCap = (int) kOutputRingFrames;
    if (streamGuitarScratch.getNumSamples() < streamScratchCap) streamGuitarScratch.setSize(2, streamScratchCap, false, false, true);
    streamSink.prepareProducerScratch();
    if (rendererBusPullScratch.getNumSamples() < streamScratchCap) rendererBusPullScratch.setSize(2, streamScratchCap, false, false, true);
    if (mixerChannelScratch.getNumSamples() < streamScratchCap) mixerChannelScratch.setSize(2, streamScratchCap, false, false, true);
    // NOTE: outputBackingBuffer is sized by audioDeviceAboutToStart() from the
    // INPUT device's block size — it's the split-input DSP scratch, not an
    // output-side buffer. Don't touch it here: resizing from the output
    // thread races with the input callback and can shrink the scratch below
    // its expected size. The output side uses backingBuffer / backingInputBuffer,
    // sized below.

    // Prefer the output device's reported rate as authoritative. The
    // stored currentSampleRate (set from the input side) was the right
    // fallback during initial setup, but if the OS or driver reopened
    // the output device at a different rate (format change, sleep/resume,
    // user-changed default), trusting the stored value would seed the
    // stretcher with a mismatched rate. Compare and warn so the
    // discrepancy is visible in logs; the device-reported rate wins.
    const double srStored = currentSampleRate.load(std::memory_order_relaxed);
    const double srDev    = device->getCurrentSampleRate();
    const double sr = srDev > 0.0 ? srDev : srStored;
    if (srStored > 0.0 && srDev > 0.0 && std::abs(srStored - srDev) > 0.5)
    {
        fprintf(stderr, "[AudioEngine] audioOutputAboutToStart: stored sr=%.0f differs from device sr=%.0f — using device\n",
                srStored, srDev);
    }
    // Propagate the authoritative rate to currentSampleRate so downstream
    // consumers (audioOutputCallback's latency comp, getLatencyMs(),
    // scoreChord's fallback) all see the same value. Without this, a
    // device-side rate change (sleep/resume, format change) would leave
    // currentSampleRate stuck at the input-side seed value.
    if (sr > 0.0) currentSampleRate.store(sr, std::memory_order_relaxed);
    // The output device drives backing playback in split mode, so this is
    // where the stretcher gets sized for that side.
    backing.prepare(sr, bs);
}

void AudioEngine::audioOutputStopped()
{
    if (slopsmith_vst_trace::isEnabled())
        fprintf(stderr, "[diag] audioOutputStopped\n");
    // No-op by design. The consumer's catch-up branch in audioOutputCallback
    // handles both (w - r) > cap (producer lapped during the stop) and
    // w < r (a future reset race) on the next output start, so we don't
    // need to reset readIndex here. Resetting r to 0 while the producer
    // keeps advancing w is equivalent — both end up in the catch-up branch
    // — but it leaves a transient window where r appears reset without the
    // ring invariants being re-established, which is harder to reason about.
}

// ── Streamer mix output sink — moved to engine/StreamSink.{h,cpp} (TLC
// phase 2). Facades below keep the NodeAddon-visible surface unchanged.

juce::String AudioEngine::setStreamOutputDevice(const juce::String& typeName, const juce::String& deviceName)
{
    return streamSink.open(typeName, deviceName);
}

void AudioEngine::clearStreamOutput()
{
    streamSink.clear();
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputData, int numInputChannels,
    float* const* outputData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Flush denormals (FTZ/DAZ) for the ENTIRE realtime callback. The chain is
    // full of IIR state (NAM, cab IRs, VST amps/EQ/comp); after each note that
    // state decays toward zero and lands in the denormal range, where every op
    // is 10-100× slower — producing sporadic CPU spikes → buffer underruns heard
    // as random "scratches" + frame stutter. Scoped so it only affects this path.
    const juce::ScopedNoDenormals noDenormals;

    // Publish that the callback body is executing so removeSource() and deferred-
    // release reclamation know when no source is being processed (the body is
    // quiescent) and a removed source can be safely released. Index 0 = primary.
    const slopsmith::SourcePool::CallbackGuard cbGuard(pool, 0);
    const int inFlightBefore = cbGuard.previousInFlight;

    // DIAG: two primary callback bodies at once = the input callback is
    // registered twice on the device manager (the half-speed/garble bug) or a
    // second device is dispatching into the primary path. Should never fire.
    if (inFlightBefore > 0 && audiodiag::firstN(audiodiag::primaryReentry))
        fprintf(stderr, "[diag] PRIMARY CALLBACK RE-ENTERED (inFlight=%d, numSamples=%d) — duplicate registration?\n",
                inFlightBefore + 1, numSamples);

    // DIAG: block larger than the size everything was prepared with.
    {
        const int preparedBs = inputBlockSize.load(std::memory_order_relaxed);
        if (preparedBs > 0 && numSamples > preparedBs && audiodiag::firstN(audiodiag::oversizedBlocks))
            fprintf(stderr, "[diag] primary callback OVERSIZED block: numSamples=%d > prepared=%d\n",
                    numSamples, preparedBs);
    }

    const bool duplex = duplexMode.load(std::memory_order_relaxed);

    // Duplex writes outputData directly. Split runs DSP into a private 2-channel
    // scratch and pushes the result to outputRing for OutputCallback.
    juce::AudioBuffer<float> buffer;
    if (duplex)
    {
        buffer.setDataToReferTo(outputData, numOutputChannels, numSamples);
    }
    else
    {
        // outputBackingBuffer is pre-sized to inputBlockSize in
        // audioDeviceAboutToStart(); never realloc on the RT thread. A
        // reconfig race could transiently deliver a larger numSamples —
        // clamp it (drop the tail) so the rest of the callback operates
        // strictly within the allocated scratch.
        const int scratchCap = outputBackingBuffer.getNumSamples();
        if (numSamples > scratchCap)
            numSamples = scratchCap;
        outputBackingBuffer.clear(0, 0, numSamples);
        outputBackingBuffer.clear(1, 0, numSamples);
        buffer.setDataToReferTo(outputBackingBuffer.getArrayOfWritePointers(), 2, numSamples);
    }

    const int effectiveOutputChannels = duplex ? numOutputChannels : 2;

    // Per-source capture + detect + monitor.
    //
    // Fast path — exactly one active source: process it in place on `buffer`,
    // byte-identical to the single-pipeline engine (channel select / mono mix +
    // input gain, input metering, ML + input-ring feed, noise gate, YIN + raw-ring
    // feed, tone chain + NaN scrub, monitor mute, chain gain, tone polish).
    //
    // Multi-source path: each active source renders its own 2-channel monitor into
    // sourceMonitorScratch (each builds its mono from its bound input channel +
    // feeds its own rings/detectors/verifier), and the monitors are summed into the
    // output. Each source scores its OWN arrangement independently.
    // This is the PRIMARY input device's callback: it mixes only sources bound to
    // device 0 (deviceKey == 0). Sources bound to an additional input device
    // (Phase 2) are processed by that device's own callback on its own hardware
    // clock — never here. With no extra device every source is deviceKey 0, so
    // this is the single-device path unchanged. See mixSourcesForDevice for the
    // fast-path / multi-source rationale; it is shared with each extra callback.
    pool.mixForDevice(0, inputData, numInputChannels, buffer, sourceMonitorScratch,
                      effectiveOutputChannels, numSamples);

    // Duplex mixes backing + applies output gain + meters here.
    // Split defers all three to OutputCallback (output device's clock).
    if (duplex)
    {
        // Stream sink (producer): snapshot the guitar monitor mix BEFORE backing is
        // added, so the stream submix can carry guitar independent of the local mix.
        const bool streamActive = streamSink.isActive();
        int   streamBackingFrames = 0;
        float streamBackingVol    = 0.0f;
        bool  streamBackingOn     = false;
        if (streamActive && streamGuitarScratch.getNumSamples() >= numSamples)
            for (int ch = 0; ch < 2; ++ch)
                streamGuitarScratch.copyFrom(ch, 0, buffer,
                    juce::jmin(ch, buffer.getNumChannels() - 1), 0, numSamples);

        const juce::ScopedTryLock sl(backing.getLock());
        if (sl.isLocked() && backing.readyLocked())
        {
            const int outSamples = backing.renderBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            streamBackingFrames = outSamples; streamBackingVol = bVol; streamBackingOn = true;
            const int mixChannels = juce::jmin(numOutputChannels, 2);
            float backingLevelSq = 0.0f;
            for (int ch = 0; ch < mixChannels; ++ch)
            {
                const float* const src = backing.renderBuffer().getReadPointer(ch);
                float sumSquares = 0.0f;
                for (int i = 0; i < outSamples; ++i)
                    sumSquares += src[i] * src[i];

                const float channelRms = (outSamples > 0)
                    ? std::sqrt(sumSquares / outSamples) * bVol
                    : 0.0f;
                backingLevelSq += channelRms * channelRms;
                buffer.addFrom(ch, 0, backing.renderBuffer(), ch, 0, outSamples, bVol);
            }
            currentBackingLevel.store((mixChannels > 0)
                ? std::sqrt(backingLevelSq / mixChannels)
                : 0.0f);
        }
        else
        {
            currentBackingLevel.store(0.0f);
        }

        // Renderer bus (duplex clock): pulled ONCE per block into the scratch,
        // then shared by the stream submix and the device output — the ring is
        // a single-consumer SPSC, so the stream path must not drain it again.
        const int rendererFrames = pullRendererBus(rendererBusPullScratch, numSamples);

        // Stream sink: compose + push the stream submix (guitar snapshot +
        // backing + renderer-bus song audio) BEFORE the local master output
        // gain, so the stream level is independent.
        if (streamActive)
            streamSink.publish(streamGuitarScratch,
                                    streamBackingOn ? &backing.renderBuffer() : nullptr,
                                    streamBackingFrames, streamBackingVol,
                                    rendererFrames > 0 ? &rendererBusPullScratch : nullptr,
                                    rendererFrames, numSamples);

        // Renderer bus into the device output — like backing, before master gain.
        if (rendererFrames > 0)
            for (int ch = 0; ch < juce::jmin(numOutputChannels, 2); ++ch)
                buffer.addFrom(ch, 0, rendererBusPullScratch, ch, 0, rendererFrames);

        // Apply output gain
        buffer.applyGain(outputGain.load());

        // Output metering
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentOutputLevel.store(peak);
        float prevPeak = outputPeak.load();
        if (peak > prevPeak) outputPeak.store(peak);
    }
    else
    {
        // Split: push processed stereo (pre-backing, pre-output-gain) into the
        // primary ring. OutputCallback adds backing + output gain on its own clock
        // and sums every extra-device ring alongside this one.
        outputRing.push(buffer.getReadPointer(0), buffer.getReadPointer(1), numSamples);
    }

    // Body done (CallbackGuard dtor) — pairs with remove()/reclaim acquire loads.
}

int AudioEngine::activeExtraInputCount() const
{
    return extraInputs.activeCount();
}

juce::String AudioEngine::bindInputDevice(int deviceKey, const juce::String& deviceName)
{
    return extraInputs.bind(deviceKey, deviceName);
}

bool AudioEngine::unbindInputDevice(int deviceKey)
{
    return extraInputs.unbind(deviceKey);
}

void AudioEngine::audioOutputCallback(const float* const* /*inputData*/,
                                      int /*numInputChannels*/,
                                      float* const* outputData,
                                      int numOutputChannels,
                                      int numSamples)
{
    // Split-mode output clock: this callback renders the backing track (phase
    // vocoder + loudness leveler) and mixes it with the chain output. Those
    // carry IIR/decay state too, so flush denormals here as well — the primary
    // callback's ScopedNoDenormals does NOT reach this separate output thread.
    const juce::ScopedNoDenormals noDenormals;

    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);
    if (numOutputChannels <= 0)
        return;

    if ((int) outputPullScratchL.size() < numSamples && audiodiag::firstN(audiodiag::outputOversized))
        fprintf(stderr, "[diag] output callback OVERSIZED block: numSamples=%d > scratch=%d\n",
                numSamples, (int) outputPullScratchL.size());

    // Clamp the working size to the scratch capacity pre-allocated in
    // audioOutputAboutToStart() so the .assign() calls below never realloc
    // on the RT thread when a transient oversized block arrives (mirrors
    // the backing path's clamp in audioDeviceIOCallbackWithContext).
    const int scratchCap = (int) outputPullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = outputRing.readIndex.load(std::memory_order_relaxed);
    const uint64_t w = outputRing.writeIndex.load(std::memory_order_acquire);

    // Resync if audioDeviceStopped() raced between our two loads and reset
    // the indices; catch up (drop-oldest) if the producer lapped — both moves
    // live on PackedStereoRing now, same semantics as before.
    outputRing.resyncIfIndicesReset(r, w);
    if (outputRing.catchUpIfLapped(r, w))
        inputOverflowCount.fetch_add(1, std::memory_order_relaxed);

    const uint64_t available = w - r;
    const int      pullCount = juce::jmin(outSamples, (int) available);

    // When scratchCap clamped outSamples below numSamples (device-reconfig
    // race), we still need to consume the ring frames that match the
    // device callback's full block size — otherwise those extras stay
    // queued and play back late, accumulating ring/output-clock skew
    // until the latency is audible. Drop them from the ring without
    // copying into scratch.
    const int      consumeCount = juce::jmin(numSamples, (int) available);

    for (int i = 0; i < pullCount; ++i)
    {
        // Single atomic load → atomic unpack of both channels (matches the
        // producer's packed store) so L and R always belong to the same frame.
        float l, rr;
        outputRing.readFrame(r + (uint64_t) i, l, rr);
        outputPullScratchL[(size_t) i] = l;
        outputPullScratchR[(size_t) i] = rr;
    }
    if (pullCount < outSamples)
    {
        for (int i = pullCount; i < outSamples; ++i)
        {
            outputPullScratchL[(size_t) i] = 0.0f;
            outputPullScratchR[(size_t) i] = 0.0f;
        }
        outputUnderflowCount.fetch_add(1, std::memory_order_relaxed);
    }
    outputRing.commitRead(r + (uint64_t) consumeCount);

    buffer.clear();
    const int copyChannels = juce::jmin(numOutputChannels, 2);
    for (int i = 0; i < outSamples; ++i)
    {
        buffer.setSample(0, i, outputPullScratchL[(size_t) i]);
        if (copyChannels > 1)
            buffer.setSample(1, i, outputPullScratchR[(size_t) i]);
    }

    // Phase 2: sum every active EXTRA input device's ring on top of the primary's
    // output. Each is an independent SPSC ring fed by that device's own callback at
    // its own hardware clock; the same drop-oldest catch-up absorbs its drift, so
    // two separate interfaces mix cleanly with no cross-device resampling.
    for (auto& s : extraInputs.slots)
    {
        if (! s.active.load(std::memory_order_acquire)) continue;
        uint64_t er = s.ring.readIndex.load(std::memory_order_relaxed);
        const uint64_t ew = s.ring.writeIndex.load(std::memory_order_acquire);
        s.ring.resyncIfIndicesReset(er, ew);
        if (s.ring.catchUpIfLapped(er, ew))
            s.overflowCount.fetch_add(1, std::memory_order_relaxed);
        const uint64_t eAvail   = ew - er;
        const int      ePull    = juce::jmin(outSamples, (int) eAvail);
        const int      eConsume = juce::jmin(numSamples,  (int) eAvail);
        for (int i = 0; i < ePull; ++i)
        {
            float l, rr;
            s.ring.readFrame(er + (uint64_t) i, l, rr);
            buffer.addSample(0, i, l);
            if (copyChannels > 1) buffer.addSample(1, i, rr);
        }
        s.ring.commitRead(er + (uint64_t) eConsume);
    }

    // Stream sink (producer, split clock): snapshot the full guitar mix (primary
    // ring + extra inputs) BEFORE backing is added.
    const bool streamActive = streamSink.isActive();
    int   streamBackingFrames = 0;
    float streamBackingVol    = 0.0f;
    bool  streamBackingOn     = false;
    if (streamActive && streamGuitarScratch.getNumSamples() >= numSamples)
        for (int ch = 0; ch < 2; ++ch)
            streamGuitarScratch.copyFrom(ch, 0, buffer,
                juce::jmin(ch, buffer.getNumChannels() - 1), 0, numSamples);

    {
        const juce::ScopedTryLock sl(backing.getLock());
        if (sl.isLocked() && backing.readyLocked())
        {
            // Shared with the duplex path so the two callbacks can't drift.
            const int backingOut = backing.renderBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            streamBackingFrames = backingOut; streamBackingVol = bVol; streamBackingOn = true;
            // RMS, computed identically to the duplex path so getBackingLevel()
            // reports the same metric regardless of which device clock is active.
            // copyChannels is already capped at 2, so it doubles as the mix count.
            float backingLevelSq = 0.0f;
            for (int ch = 0; ch < copyChannels; ++ch)
            {
                const float* const src = backing.renderBuffer().getReadPointer(ch);
                float sumSquares = 0.0f;
                for (int i = 0; i < backingOut; ++i)
                    sumSquares += src[i] * src[i];

                const float channelRms = (backingOut > 0)
                    ? std::sqrt(sumSquares / backingOut) * bVol
                    : 0.0f;
                backingLevelSq += channelRms * channelRms;
                buffer.addFrom(ch, 0, backing.renderBuffer(), ch, 0, backingOut, bVol);
            }
            currentBackingLevel.store((copyChannels > 0)
                ? std::sqrt(backingLevelSq / copyChannels)
                : 0.0f);
        }
        else
        {
            currentBackingLevel.store(0.0f);
        }

        // Renderer bus (split clock): pulled ONCE per block into the scratch,
        // shared by the stream submix and the device output (single-consumer
        // ring — the stream path must not drain it again).
        const int rendererFrames = pullRendererBus(rendererBusPullScratch, numSamples);

        // Stream sink: compose + push the stream submix before the local master
        // gain. Done INSIDE the backingLock scope so backingBuffer is read under the
        // very lock that guards its resize (audio*AboutToStart) — matching the duplex
        // path. When the tryLock failed (streamBackingOn=false) we pass a null backing
        // pointer and never touch backingBuffer, so there is nothing to protect.
        if (streamActive)
            streamSink.publish(streamGuitarScratch,
                                    streamBackingOn ? &backing.renderBuffer() : nullptr,
                                    streamBackingFrames, streamBackingVol,
                                    rendererFrames > 0 ? &rendererBusPullScratch : nullptr,
                                    rendererFrames, numSamples);

        // Renderer bus into the device output — like backing, before master gain.
        if (rendererFrames > 0)
            for (int ch = 0; ch < juce::jmin(copyChannels, 2); ++ch)
                buffer.addFrom(ch, 0, rendererBusPullScratch, ch, 0, rendererFrames);
    }

    buffer.applyGain(outputGain.load());

    float peak = 0.0f;
    for (int ch = 0; ch < numOutputChannels; ++ch)
        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
    currentOutputLevel.store(peak);
    float prevPeak = outputPeak.load();
    if (peak > prevPeak) outputPeak.store(peak);
}

// ── Renderer-audio bus (Phase 2: WebAudio master → engine output) ────────────

bool AudioEngine::pushRendererAudio(const float* interleavedLR, int frames, double sourceRate)
{
    // Producer-side resample + publish live on channel #0's bus (the
    // renderer-master default channel — engine/Mixer.h).
    return mixer.defaultBus().push(interleavedLR, frames, sourceRate, getCurrentSampleRate());
}

int AudioEngine::pullRendererBus(juce::AudioBuffer<float>& dest, int numSamples)
{
    // Cold start before about-to-start sized the scratches — skip, never
    // alloc on the RT thread (same rule as the stream scratches).
    if (dest.getNumSamples() < numSamples || dest.getNumChannels() < 2) return 0;
    if (mixerChannelScratch.getNumSamples() < numSamples || mixerChannelScratch.getNumChannels() < 2) return 0;
    dest.clear(0, 0, numSamples);
    dest.clear(1, 0, numSamples);
    const int contributed = mixer.pullMixInto(dest.getWritePointer(0), dest.getWritePointer(1),
                                              numSamples,
                                              mixerChannelScratch.getWritePointer(0),
                                              mixerChannelScratch.getWritePointer(1));
    return contributed > 0 ? numSamples : 0;
}

AudioEngine::RendererBusMetrics AudioEngine::getRendererBusMetrics() const
{
    const auto bm = mixer.defaultBus().metrics();
    RendererBusMetrics m;
    m.pushedFrames   = bm.pushedFrames;
    m.consumedFrames = bm.consumedFrames;
    m.underflowCount = bm.underflowCount;
    m.overflowCount  = bm.overflowCount;
    m.fillFrames     = bm.fillFrames;
    m.capacityFrames = bm.capacityFrames;
    m.enabled        = bm.enabled;
    return m;
}
