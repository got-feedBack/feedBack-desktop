#include "AudioEngine.h"
#include "AudioSanitize.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// Hard ceiling on backing playback speed. This drives input buffer sizing and runtime clamp.
static constexpr double kMaxBackingSpeed = 4.0;
// Transparent full-speed path — skip the stretcher when rate is effectively 1×.
static constexpr double kBackingSpeedBypassEpsilon = 1.0e-4;

// On Windows, ASIO drivers can crash with access violations.
// We catch C++ exceptions but can't easily catch SEH in functions with dtors.
// The try/catch blocks around device operations are the best we can do
// without restructuring the code into SEH-safe wrapper functions.

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    // Start the backing read-ahead worker so the transport's BufferingAudioSource
    // always has a live thread to pull decoded audio on. It sleeps while idle and
    // costs nothing until a track is loaded.
    backingReadThread.startThread();

    // Construct the full source pool up front so addSource/removeSource never
    // reassign a pointer the audio thread reads — they only flip `active`. Each
    // chain reads the engine's audioRunning / currentSampleRate atomics by
    // reference (both already constructed as members before this body runs).
    // sources[0] is the permanent default input, active from the start; the rest
    // are inactive (no threads — NoteVerifier's worker only starts in prepare()).
    for (int i = 0; i < kMaxSources; ++i)
        sources[(size_t) i] = std::make_unique<SourceChain>(i, audioRunning, currentSampleRate);
    sources[0]->setActive(true);

    // Phase 2: tag each additional-input slot with its identity so its JUCE
    // callback can route back to the engine. deviceKey = slot index + 1 (0 is the
    // primary inputDeviceManager). The managers stay idle until bindInputDevice.
    for (int i = 0; i < kMaxExtraInputDevices; ++i)
    {
        extraInputs[(size_t) i].callback.engine = this;
        extraInputs[(size_t) i].callback.slot = i;
        extraInputs[(size_t) i].deviceKey = i + 1;
    }

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
    for (int i = 0; i < kMaxExtraInputDevices; ++i)
    {
        extraInputs[(size_t) i].manager.closeAudioDevice();
        extraInputs[(size_t) i].manager.removeAudioCallback(&extraInputs[(size_t) i].callback);
    }
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
    std::vector<BindableInput> out;
    // The device already open as the primary input IS "Main" — don't offer it as
    // an extra (would double-open the same hardware on two managers).
    juce::String primaryName;
    if (auto* dev = inputDeviceManager.getCurrentAudioDevice())
        primaryName = dev->getName();
    // Enumerate across ALL device types, not just the primary's current one —
    // bindInputDevice() can open a device under any backend (JACK/ALSA/CoreAudio/…),
    // so an extra interface exposed under a DIFFERENT backend than the primary must
    // still be offered, or the multi-device path is unreachable from the picker.
    //
    // KNOWN LIMITATION: identity is the display name. JUCE opens input devices BY
    // NAME, so two interfaces sharing a label (e.g. two identical USB cables) cannot
    // be distinguished or independently opened without a backend-specific device-id
    // rework — they collapse to one entry here. The SAME root cause makes a device
    // exposed under MULTIPLE backends (e.g. ALSA + JACK/PipeWire on Linux) ambiguous:
    // we dedup by name and bindInputDevice() re-derives the backend (preferring the
    // primary's), so we may bind the wrong backend if only another would open. A real
    // fix needs (typeName, name) identity threaded through bind/reopen. Distinct-name,
    // single-backend rigs (the common case, and the validated GP-5 + Spark setup) are
    // unaffected.
    juce::StringArray seen;
    for (auto* t : inputDeviceManager.getAvailableDeviceTypes())
    {
        if (!t) continue;
        t->scanForDevices();
        const juce::String typeName = t->getTypeName();
        for (const auto& name : t->getDeviceNames(true))
        {
            if (name == primaryName || seen.contains(name)) continue;  // dedup across backends
            // Skip monitor / loopback pseudo-inputs — not instrument inputs, only
            // confuse the picker.
            const juce::String lower = name.toLowerCase();
            if (lower.contains("monitor") || lower.contains("loopback")) continue;
            seen.add(name);
            out.push_back({ typeName, name });
        }
    }
    return out;
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
    DeviceOptions options;
    options.inputType = inputTypeName;
    options.outputType = outputTypeName.isEmpty() ? inputTypeName : outputTypeName;
    options.type = options.inputType;   // legacy alias

    // Resolve each side from its own manager so probe stays consistent with
    // applySplitSetup()/setOutputDeviceType(), which mutate the manager that
    // owns the side they're configuring. Using inputDeviceManager for the
    // output lookup would silently fall back to whatever input has scanned,
    // which can miss output-only backends.
    auto findType = [](juce::AudioDeviceManager& manager,
                       const juce::String& wanted) -> juce::AudioIODeviceType* {
        juce::AudioIODeviceType* match = nullptr;
        for (auto* type : manager.getAvailableDeviceTypes())
        {
            if ((wanted.isNotEmpty() && type->getTypeName() == wanted)
                || (wanted.isEmpty() && match == nullptr))
            {
                match = type;
                if (wanted.isNotEmpty()) break;
            }
        }
        return match;
    };

    auto* inputType  = findType(inputDeviceManager, options.inputType);

    // Match setAudioDevices's resolution: when the caller didn't specify
    // an output type, default it to the SAME type the input side resolved
    // to (using the type's name, looked up in outputDeviceManager). Without
    // this, an empty `options.outputType` would let findType pick whatever
    // outputDeviceManager enumerates first — potentially a different
    // backend than inputDeviceManager picked from the empty string, which
    // then disagrees with the apply path's duplex classification.
    juce::String effectiveOutputTypeName = options.outputType;
    if (effectiveOutputTypeName.isEmpty() && inputType != nullptr)
        effectiveOutputTypeName = inputType->getTypeName();
    auto* outputType = findType(outputDeviceManager, effectiveOutputTypeName);

    if (inputType == nullptr)
    {
        options.error = "Input device type not found";
        options.compatible = false;
        return options;
    }
    if (outputType == nullptr)
    {
        options.error = "Output device type not found";
        options.compatible = false;
        return options;
    }

    try
    {
        options.inputType = inputType->getTypeName();
        options.outputType = outputType->getTypeName();
        options.type = options.inputType;

        options.input = inputName;
        options.output = outputName;

        // userIntendsDuplex matches setAudioDevices's classification:
        // identical names on both sides (typically both empty = OS default)
        // means we'll go duplex regardless of which specific devices the
        // first-enumerated lookup would have produced.
        const bool userIntendsDuplex = (options.inputType == options.outputType
                                        && options.input == options.output);

        // For probing we still need a concrete device to instantiate.
        // Resolve empty names to first-enumerated ONLY for the probe-device
        // creation below — DON'T write back into options.input/options.output;
        // those flow to the UI and the apply path, which treat empty as
        // "OS default" per side.
        auto inputs  = inputType->getDeviceNames(true);
        auto outputs = outputType->getDeviceNames(false);
        const juce::String probeInputName =
            options.input.isEmpty() && inputs.size() > 0 ? inputs[0] : options.input;
        const juce::String probeOutputName =
            options.output.isEmpty() && outputs.size() > 0 ? outputs[0] : options.output;

        const bool isDuplex = userIntendsDuplex
                              || (options.inputType == options.outputType
                                  && options.input == options.output
                                  && options.input.isNotEmpty());

        if (isDuplex)
        {
            std::unique_ptr<juce::AudioIODevice> dev(
                inputType->createDevice(probeOutputName, probeInputName));
            if (!dev) { options.error = "Could not create probe device"; options.compatible = false; return options; }

            options.inputChannels = dev->getInputChannelNames();
            options.outputChannels = dev->getOutputChannelNames();
            for (auto rate : dev->getAvailableSampleRates())
                options.sampleRates.addIfNotAlreadyThere(rate);
            for (auto size : dev->getAvailableBufferSizes())
                options.bufferSizes.addIfNotAlreadyThere(size);
        }
        else
        {
            std::unique_ptr<juce::AudioIODevice> inDev(
                inputType->createDevice({}, probeInputName));
            std::unique_ptr<juce::AudioIODevice> outDev(
                outputType->createDevice(probeOutputName, {}));
            if (!inDev || !outDev)
            {
                options.error = "Could not create dual probe devices";
                options.compatible = false;
                return options;
            }

            options.inputChannels = inDev->getInputChannelNames();
            options.outputChannels = outDev->getOutputChannelNames();

            // Tolerance covers backends that report fractional drift around the nominal rate.
            const auto inRates = inDev->getAvailableSampleRates();
            const auto outRates = outDev->getAvailableSampleRates();
            for (auto r : inRates)
            {
                for (auto r2 : outRates)
                {
                    // <= 0.5 (not <) to match applySplitSetup's rateSupportedBy
                    // check. A backend reporting 47999.5 on both sides has
                    // |r - r2| = 0 (matches anyway) but a backend mixing
                    // 47999.5 in / 48000.0 out has |diff| = 0.5 exactly, which
                    // < 0.5 would reject from the probe even though the
                    // apply-side check accepts it.
                    if (std::abs(r - r2) <= 0.5)
                    {
                        // Round the midpoint to a clean nominal rate
                        // (backends sometimes report fractional near-48000
                        // rates; surfacing the raw value would fail the
                        // apply-side setAudioDeviceSetup, which expects an
                        // exact supported nominal). Re-check the rounded
                        // candidate is within tolerance of BOTH sides — a
                        // matched pair like 48000.4/48000.6 passes the |r-r2|
                        // check but std::round(48000.4)=48000 would fall
                        // outside tolerance of 48000.6 (diff 0.6). Skip
                        // those so the probe stays fail-closed.
                        const double candidate = std::round((r + r2) * 0.5);
                        if (std::abs(r  - candidate) <= 0.5
                         && std::abs(r2 - candidate) <= 0.5)
                        {
                            options.sampleRates.addIfNotAlreadyThere(candidate);
                        }
                        break;
                    }
                }
            }
            if (options.sampleRates.isEmpty())
            {
                options.error = "Input and output devices share no common sample rate";
                options.compatible = false;
            }

            // Split mode opens both sides with the same bufferSize, so the
            // UI should only see sizes the intersection of both devices
            // supports — a union would let the user pick a value that
            // predictably fails at apply time on one side.
            const auto inBufs = inDev->getAvailableBufferSizes();
            const auto outBufs = outDev->getAvailableBufferSizes();
            for (auto b : inBufs)
            {
                for (auto b2 : outBufs)
                {
                    if (b == b2)
                    {
                        options.bufferSizes.addIfNotAlreadyThere(b);
                        break;
                    }
                }
            }
            // An empty intersection means there's no buffer size both sides
            // accept; setting compatible=false stops the UI from re-enabling
            // Apply against a guaranteed-fail config.
            if (options.bufferSizes.isEmpty() && options.error.isEmpty())
            {
                options.error = "Input and output devices share no common buffer size";
                options.compatible = false;
            }
        }

        fprintf(stderr, "[AudioEngine] Probed device options: inType='%s' outType='%s' in='%s' out='%s' "
                "duplex=%d inputs=%d outputs=%d rates=%d buffers=%d compatible=%d\n",
                options.inputType.toRawUTF8(), options.outputType.toRawUTF8(),
                options.input.toRawUTF8(), options.output.toRawUTF8(),
                (int) isDuplex, options.inputChannels.size(), options.outputChannels.size(),
                options.sampleRates.size(), options.bufferSizes.size(), (int) options.compatible);
    }
    catch (const std::exception& e)
    {
        options.error = e.what();
        options.compatible = false;
    }
    catch (...)
    {
        options.error = "Probe failed";
        options.compatible = false;
    }

    return options;
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
        const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);
        const uint64_t r = outputRingReadIndex.load(std::memory_order_acquire);
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
    const bool wasRunning = audioRunning.load(std::memory_order_relaxed);

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

    // User-intent duplex: both sides came in identical (typically both
    // empty = "system default", or both naming the same explicit device).
    // Capture before we resolve names, otherwise the resolve loop below
    // fills empty-input with first-input-device and empty-output with
    // first-output-device — those usually differ (especially on macOS
    // where defaults are separate input/output devices), and the engine
    // would silently route into split mode with ~85ms of ring-buffer
    // latency for a config the user expected to be duplex. Legacy
    // pre-PR settings commonly use empty names; preserve their behavior.
    const bool userIntendsDuplex = (resolvedInputType == resolvedOutputType
                                    && config.inputDevice == config.outputDevice);

    // Don't resolve empty names to first-device-of-each-type. Pre-PR
    // behavior — and Copilot's fail-closed concern — treat empty names
    // as "OS default" per side. Filling them with inputs[0] / outputs[0]
    // is JUCE-enumeration-order dependent and can pick the wrong device
    // (e.g. an audio interface that isn't the system default). Both
    // applyDuplexSetup and applySplitSetup handle empty names by setting
    // useDefault*Channels=true, letting JUCE select the OS default.
    const juce::String& resolvedInput  = config.inputDevice;
    const juce::String& resolvedOutput = config.outputDevice;

    const bool isDuplex = userIntendsDuplex
                          || (resolvedInputType == resolvedOutputType
                              && resolvedInput == resolvedOutput
                              && resolvedInput.isNotEmpty());

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

    if (isDuplex)
    {
        teardownSplitMode();

        const juce::String err = applyDuplexSetup(resolvedInput, resolvedOutput,
                                                  requestedSampleRate, requestedBufferSize);
        if (err.isNotEmpty())
        {
            res.error = err;
            res.duplex = true;
            return res;
        }
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
        DeviceConfig resolved = config;
        resolved.inputType = resolvedInputType;
        resolved.outputType = resolvedOutputType;
        resolved.inputDevice = resolvedInput;
        resolved.outputDevice = resolvedOutput;
        resolved.sampleRate = requestedSampleRate;
        resolved.bufferSize = requestedBufferSize;

        res = applySplitSetup(resolved);
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

juce::String AudioEngine::applyDuplexSetup(const juce::String& inputName,
                                           const juce::String& outputName,
                                           double sampleRate, int bufferSize)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputName;
    setup.outputDeviceName = outputName;
    setup.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    setup.bufferSize = bufferSize > 0 ? bufferSize : 256;
    setup.useDefaultInputChannels = inputName.isEmpty();
    setup.useDefaultOutputChannels = outputName.isEmpty();

    // Channel masks must match too — high-numbered selectedInputChannel needs
    // the expanded mask that an older session may not have opened.
    if (auto* currentDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        try
        {
            juce::AudioDeviceManager::AudioDeviceSetup current;
            inputDeviceManager.getAudioDeviceSetup(current);

            const int advertisedInputs = currentDevice->getInputChannelNames().size();
            juce::BigInteger expectedInputs;
            expectedInputs.setRange(0, advertisedInputs > 0 ? advertisedInputs : 2, true);

            const int advertisedOutputs = currentDevice->getOutputChannelNames().size();
            juce::BigInteger expectedOutputs;
            expectedOutputs.setRange(0, juce::jmin(advertisedOutputs > 0 ? advertisedOutputs : 2, 2), true);

            if (current.inputDeviceName == setup.inputDeviceName
                && current.outputDeviceName == setup.outputDeviceName
                && current.sampleRate == setup.sampleRate
                && current.bufferSize == setup.bufferSize
                && current.useDefaultInputChannels == setup.useDefaultInputChannels
                && current.useDefaultOutputChannels == setup.useDefaultOutputChannels
                && current.inputChannels == expectedInputs
                && current.outputChannels == expectedOutputs
                && duplexMode.load(std::memory_order_relaxed))
            {
                fprintf(stderr, "[AudioEngine] Duplex device already configured with same settings, skipping\n");
                return {};
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed (unknown)\n");
        }
    }

    // ALSA deadlocks on reconfigure unless we fully close first. WASAPI
    // reconfigures in place and is much slower if closed.
#if JUCE_LINUX
    juce::String currentTypeName;
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();
    if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
    {
        try {
            inputDeviceManager.closeAudioDevice();
            fprintf(stderr, "[AudioEngine] Closed device for reconfiguration\n");
            if (currentTypeName.isNotEmpty())
                inputDeviceManager.setCurrentAudioDeviceType(currentTypeName, true);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] closeAudioDevice crashed, continuing\n");
        }
    }
#endif

    int inputChannelCount = 0;
    int outputChannelCount = 0;
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        try
        {
            if (auto probe = std::unique_ptr<juce::AudioIODevice>(type->createDevice(outputName, inputName)))
            {
                inputChannelCount = probe->getInputChannelNames().size();
                outputChannelCount = probe->getOutputChannelNames().size();
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed (unknown)\n");
        }
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    if (outputChannelCount <= 0) outputChannelCount = 2;

    setup.inputChannels.setRange(0, inputChannelCount, true);
    setup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    juce::String result;
    try {
        result = inputDeviceManager.setAudioDeviceSetup(setup, true);
    } catch (...) {
        return "setAudioDeviceSetup threw";
    }
    if (result.isNotEmpty())
    {
        fprintf(stderr, "[AudioEngine] Device setup error: %s\n", result.toRawUTF8());
        try {
            result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
        } catch (...) {
            return "fallback initialiseWithDefaultDevices threw";
        }
        if (result.isNotEmpty())
            return "device setup failed: " + result;
    }

    if (auto* configuredDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        const double sr = configuredDevice->getCurrentSampleRate();
        const int bs = configuredDevice->getCurrentBufferSizeSamples();
        currentSampleRate.store(sr, std::memory_order_relaxed);
        inputBlockSize.store(bs, std::memory_order_relaxed);
        outputBlockSize.store(bs, std::memory_order_relaxed);

        fprintf(stderr, "[AudioEngine] Duplex device configured OK. Current device: %s\n",
                configuredDevice->getName().toRawUTF8());
        fprintf(stderr, "[AudioEngine] Actual device setup: sr=%.0f bs=%d (requested bs=%d)\n",
                sr, bs, bufferSize);

        source0().prepareMonitorChain(sr, bs);
        return {};
    }
    currentSampleRate.store(0.0, std::memory_order_relaxed);
    inputBlockSize.store(0, std::memory_order_relaxed);
    outputBlockSize.store(0, std::memory_order_relaxed);
    source0().releaseMonitorChain();
    return "no current device after setup";
}

AudioEngine::DeviceConfigResult AudioEngine::applySplitSetup(const DeviceConfig& config)
{
    DeviceConfigResult res;
    res.duplex = false;

    // The split-mode output ring is fixed at kOutputRingFrames samples
    // (~85ms @ 48kHz). A single callback at bufferSize > kOutputRingFrames
    // would overrun the ring in one go, guaranteeing immediate
    // overwrite/wrap and audible glitches. Reject those configurations up
    // front — duplex still works fine since it bypasses the ring entirely.
    if (config.bufferSize > kOutputRingFrames)
    {
        res.error = "Buffer size " + juce::String(config.bufferSize)
                  + " exceeds split-mode ring capacity ("
                  + juce::String(kOutputRingFrames) + "). Pick a smaller buffer size or use duplex.";
        return res;
    }

    // setCurrentAudioDeviceType can throw from JUCE backends (ASIO).
    // Catch so the failure surfaces as a structured error rather than an
    // exception crossing the N-API boundary.
    try
    {
        if (auto* current = outputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != config.outputType)
                outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
        else
        {
            outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for output type '" + config.outputType + "'";
        return res;
    }

    // v1 forces matching nominal SR — no adaptive resampler yet.
    // Resolve empty name to first-enumerated for the createDevice probe
    // call (matches probeDeviceOptionsDual's strategy). createDevice("")
    // is implementation-defined per backend — some return the default,
    // some return null. Using first-enumerated keeps probe and apply
    // checking the SAME concrete device, so an empty-name config can't
    // pass the UI probe and then fail this check.
    auto rateSupportedBy = [&](juce::AudioIODeviceType* t,
                               const juce::String& dev, bool isInput, double sr) {
        if (!t) return false;
        juce::String resolved = dev;
        if (resolved.isEmpty())
        {
            auto names = t->getDeviceNames(isInput);
            if (names.size() > 0) resolved = names[0];
        }
        std::unique_ptr<juce::AudioIODevice> probe(
            isInput ? t->createDevice({}, resolved) : t->createDevice(resolved, {}));
        if (!probe) return false;
        // Tolerance matches the probe-side rounding: probeDeviceOptionsDual
        // rounds the matched rate to the nearest integer (see :208), so a
        // backend reporting e.g. 47999.5 surfaces 48000 in the UI. If we
        // kept `< 0.5` here, the round-trip would fail at apply time because
        // |47999.5 - 48000.0| is exactly 0.5. Use `<= 0.5` so the boundary
        // case the probe accepted is also accepted at apply.
        for (auto r : probe->getAvailableSampleRates())
            if (std::abs(r - sr) <= 0.5) return true;
        return false;
    };
    juce::AudioIODeviceType* inputType = nullptr;
    juce::AudioIODeviceType* outputType = nullptr;
    for (auto* t : inputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.inputType) { inputType = t; break; }
    for (auto* t : outputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.outputType) { outputType = t; break; }
    if (!inputType || !outputType)
    {
        res.error = "Device type not found";
        return res;
    }
    if (!rateSupportedBy(inputType, config.inputDevice, true, config.sampleRate)
     || !rateSupportedBy(outputType, config.outputDevice, false, config.sampleRate))
    {
        res.error = "Sample rate not supported by both input and output devices";
        return res;
    }

    juce::AudioDeviceManager::AudioDeviceSetup inSetup;
    // Resolve empty name to first-enumerated input device — matches the
    // rateSupportedBy preflight above AND probeDeviceOptionsDual. Using
    // empty + useDefault*Channels here would make JUCE open the OS
    // default, which can differ from inputs[0] on platforms where the
    // OS-default differs from JUCE's enumeration order. The probe + SR
    // preflight + actual open all need to agree on the same concrete
    // device for the apply path to behave consistently with what the UI
    // showed the user.
    juce::String resolvedInputName = config.inputDevice;
    if (resolvedInputName.isEmpty())
    {
        auto names = inputType->getDeviceNames(true);
        if (names.size() > 0) resolvedInputName = names[0];
    }

    inSetup.inputDeviceName  = resolvedInputName;
    inSetup.outputDeviceName = "";
    inSetup.sampleRate = config.sampleRate;
    inSetup.bufferSize = config.bufferSize;
    inSetup.useDefaultInputChannels = false;
    inSetup.useDefaultOutputChannels = false;

    int inputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(inputType->createDevice({}, resolvedInputName));
            if (probe) inputChannelCount = probe->getInputChannelNames().size();
        } catch (...) {}
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    inSetup.inputChannels.setRange(0, inputChannelCount, true);
    inSetup.outputChannels.clear();

    // Rollback helper: on any failure path after a side has been opened,
    // close both managers' devices so we don't leave the OS audio resource
    // held (sometimes exclusively, e.g. ASIO) while setDevice reports a
    // failure. closeAudioDevice is idempotent so unconditional calls are
    // safe even when only the input or neither side opened.
    auto rollbackOpenedDevices = [&]() {
        // Drop any callback we already attached to the output manager —
        // closeAudioDevice() does not invoke removeAudioCallback, and leaving
        // outputCallbackRegistered=true would cause the next startAudio()
        // to skip the re-attach (it gates on !outputCallbackRegistered),
        // leaving split-mode output silent after a partial-open failure.
        if (outputCallbackRegistered)
        {
            try { outputDeviceManager.removeAudioCallback(&outputCallback); } catch (...) {}
            outputCallbackRegistered = false;
        }
        try { inputDeviceManager.closeAudioDevice(); } catch (...) {}
        try { outputDeviceManager.closeAudioDevice(); } catch (...) {}
    };

    // Mirror applyDuplexSetup's JUCE_LINUX close-before-reconfigure pattern:
    // ALSA deadlocks if we let setAudioDeviceSetup mutate a live device. The
    // device type is re-asserted afterwards so the close doesn't drop us back
    // to whatever JUCE picked at startup. closeAudioDevice/setCurrentAudioDeviceType
    // throwing is non-fatal — we still try the setup below and surface its error.
#if JUCE_LINUX
    {
        juce::String currentInputTypeName;
        if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
            currentInputTypeName = currentType->getTypeName();
        if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                inputDeviceManager.closeAudioDevice();
                if (currentInputTypeName.isNotEmpty())
                    inputDeviceManager.setCurrentAudioDeviceType(currentInputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode input close threw, continuing\n");
            }
        }
    }
#endif

    juce::String inErr;
    try { inErr = inputDeviceManager.setAudioDeviceSetup(inSetup, true); }
    catch (...) { res.error = "input setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (inErr.isNotEmpty()) { res.error = "input setup: " + inErr; rollbackOpenedDevices(); return res; }

    auto* inDev = inputDeviceManager.getCurrentAudioDevice();
    if (!inDev) { res.error = "no input device after setup"; rollbackOpenedDevices(); return res; }
    const double inSr = inDev->getCurrentSampleRate();
    const int    inBs = inDev->getCurrentBufferSizeSamples();

    // Same first-enumerated resolution on the output side — see input note
    // above for why this matches the probe + SR preflight strategy.
    juce::String resolvedOutputName = config.outputDevice;
    if (resolvedOutputName.isEmpty())
    {
        auto names = outputType->getDeviceNames(false);
        if (names.size() > 0) resolvedOutputName = names[0];
    }

    juce::AudioDeviceManager::AudioDeviceSetup outSetup;
    outSetup.inputDeviceName  = "";
    outSetup.outputDeviceName = resolvedOutputName;
    outSetup.sampleRate = config.sampleRate;
    outSetup.bufferSize = config.bufferSize;
    outSetup.useDefaultInputChannels = false;
    outSetup.useDefaultOutputChannels = false;

    int outputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(outputType->createDevice(resolvedOutputName, {}));
            if (probe) outputChannelCount = probe->getOutputChannelNames().size();
        } catch (...) {}
    }
    if (outputChannelCount <= 0) outputChannelCount = 2;
    outSetup.inputChannels.clear();
    outSetup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    // Same JUCE_LINUX close-before-reconfigure as the input side above — also
    // protects when split mode is re-applied with a different output device.
#if JUCE_LINUX
    {
        juce::String currentOutputTypeName;
        if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
            currentOutputTypeName = currentType->getTypeName();
        if (outputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                outputDeviceManager.closeAudioDevice();
                if (currentOutputTypeName.isNotEmpty())
                    outputDeviceManager.setCurrentAudioDeviceType(currentOutputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode output close threw, continuing\n");
            }
        }
    }
#endif

    juce::String outErr;
    try { outErr = outputDeviceManager.setAudioDeviceSetup(outSetup, true); }
    catch (...) { res.error = "output setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (outErr.isNotEmpty()) { res.error = "output setup: " + outErr; rollbackOpenedDevices(); return res; }

    auto* outDev = outputDeviceManager.getCurrentAudioDevice();
    if (!outDev) { res.error = "no output device after setup"; rollbackOpenedDevices(); return res; }
    const double outSr = outDev->getCurrentSampleRate();
    const int    outBs = outDev->getCurrentBufferSizeSamples();

    if (std::abs(inSr - outSr) > 0.5)
    {
        res.error = "Input and output devices opened at different sample rates";
        rollbackOpenedDevices();
        return res;
    }

    currentSampleRate.store(inSr, std::memory_order_relaxed);
    inputBlockSize.store(inBs, std::memory_order_relaxed);
    outputBlockSize.store(outBs, std::memory_order_relaxed);

    fprintf(stderr, "[AudioEngine] Split mode configured: inSr=%.0f inBs=%d outSr=%.0f outBs=%d\n",
            inSr, inBs, outSr, outBs);

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    outputUnderflowCount.store(0, std::memory_order_relaxed);
    inputOverflowCount.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);

    source0().prepareMonitorChain(inSr, inBs);

    res.ok = true;
    res.sampleRate = inSr;
    res.inputBlockSize = inBs;
    res.outputBlockSize = outBs;
    return res;
}

void AudioEngine::teardownSplitMode()
{
    // Unconditional remove — JUCE's removeAudioCallback is idempotent
    // (no-op if the callback isn't registered), so we don't need the
    // outputCallbackRegistered guard here. This makes teardown robust
    // against a stale flag left over from a previous failed split setup.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    try { outputDeviceManager.closeAudioDevice(); }
    catch (...) { fprintf(stderr, "[AudioEngine] teardownSplitMode: output close threw\n"); }

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    if (audioRunning.load(std::memory_order_relaxed))
    {
        fprintf(stderr, "[AudioEngine] startAudio: already running\n");
        return;
    }

    // Input first so it has time to prefill the ring before the output
    // callback pulls — otherwise split mode underflows once at start.
    inputDeviceManager.addAudioCallback(this);

    // Guard against double-registration: audioRunning can be cleared by
    // audioDeviceStopped() on a transient input unplug while the output
    // callback intentionally stays registered (JUCE auto-restart relies on
    // that). A later startAudio() would then add the same callback again
    // and JUCE would dispatch it twice per block.
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
    reopenDesiredExtraInputs();

    // Restore the streamer-mix output device too (same intent-survives-restart
    // pattern). Best-effort — a failure leaves the sink inactive, never blocks.
    reopenDesiredStreamSink();
}

void AudioEngine::stopAudio()
{
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
    // Extra input devices are opened independently of startAudio(); close them here
    // too so a stopped engine never leaves a second interface capturing, feeding
    // detectors, and holding the hardware open in the background. KEEP their
    // desiredDeviceName so the next startAudio() re-opens them (a stop/start cycle,
    // or a device reconfigure, restores extra inputs automatically). No-op when none
    // are bound — the single-device path is unchanged.
    for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
        closeExtraInputDevice(dk - 1);
    // Tear down the streamer-mix OUTPUT device too — KEEP its desired intent so the
    // next startAudio() reopens it (same pattern as extra inputs). Without this the
    // 2nd output device keeps running and underflowing while the engine is "stopped",
    // and the destructor (which calls stopAudio()) would be the only path that ever
    // closes it — closing the device here also makes that destructor teardown safe.
    closeStreamSinkDevice();
    audioRunning.store(false, std::memory_order_relaxed);
    currentBackingLevel.store(0.0f);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

bool AudioEngine::loadBackingTrack(const juce::File& file)
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
    backingTransport.reset();
    backingSource.reset();

    const bool exists = file.existsAsFile();
    std::cerr << "[AudioEngine] loadBackingTrack path="
              << file.getFullPathName().toStdString()
              << " exists=" << exists
              << " size=" << (exists ? (long long)file.getSize() : -1)
              << std::endl;

    auto* reader = formatManager.createReaderFor(file);
    if (!reader)
    {
        std::cerr << "[AudioEngine] loadBackingTrack: no reader for ext='"
                  << file.getFileExtension().toStdString()
                  << "' (registered formats=" << formatManager.getNumKnownFormats()
                  << ")" << std::endl;
        // Transport/source already reset above; clear cached state so the renderer
        // doesn't keep displaying the previous track's position/duration.
        cachedBackingPosition.store(0.0);
        cachedBackingDuration.store(0.0);
        return false;
    }

    const double readerSampleRate = reader->sampleRate;
    const juce::int64 readerLengthInSamples = reader->lengthInSamples;
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    // Backing audio plays through the output device in both modes, so size
    // against outputBlockSize. In duplex mode outputBlockSize == inputBlockSize;
    // in split mode the output device's clock drives the backing pull.
    const int    bs = outputBlockSize.load(std::memory_order_relaxed);

    backingSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    backingTransport = std::make_unique<juce::AudioTransportSource>();
    // Read-ahead on backingReadThread so the RT audio thread normally never
    // touches the disk or the format codec. Previously this passed
    // (…, 0, nullptr, …): with no read-ahead buffer the transport decoded the
    // file synchronously inside getNextAudioBlock ON the audio callback, so any
    // disk seek / decode spike (worst for compressed formats) blew the block
    // budget → underruns heard as glitches or brief mutes while a song plays.
    // 32768 source frames ≈ 0.68 s @ 48k of look-ahead absorbs those spikes.
    //
    // Known residual (accepted): juce::BufferingAudioSource is not fully
    // RT-safe — readBufferSection() holds callbackLock across the decode of one
    // refill chunk, and the callback's getNextAudioBlock() takes the same lock,
    // so the RT thread can still block behind an in-flight chunk decode. The
    // window is bounded (JUCE caps chunks at 2048 source frames) and only hit
    // when a refill is mid-decode, vs. the old guaranteed full decode on every
    // block; a truly lock-free ring would mean replacing the JUCE transport
    // stack and isn't worth it here.
    // The 4th arg makes AudioTransportSource SRC the file to device rate.
    // Stretch always sees device-rate audio so that its presetDefault parameters match.
    constexpr int kBackingReadAheadSamples = 32768;
    backingTransport->setSource(backingSource.get(), kBackingReadAheadSamples,
                                &backingReadThread, readerSampleRate);

    // Loading a backing track before the audio device has started leaves
    // sr/bs at zero. presetDefault(2, 0.0f) would seed the stretcher with
    // undefined internal timing, and prepareToPlay(0, 0) is similarly
    // ill-defined. Defer the stretcher + buffer setup; the relevant
    // audio*AboutToStart() re-runs the same block once a real sample
    // rate / block size are known (audioDeviceAboutToStart for duplex,
    // audioOutputAboutToStart for split).
    if (sr > 0.0 && bs > 0)
    {
        // prepareToPlay's first arg is an upper bound on subsequent
        // getNextAudioBlock requests, per the juce::AudioSource contract.
        // The RT callback can pull ceil(bs * kMaxBackingSpeed) frames in a
        // single block when the speed is above 1×, so prepare for that
        // worst case — preparing with just `bs` would risk JUCE internal
        // buffer overruns/asserts on the first faster-than-1× block.
        const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
        backingTransport->prepareToPlay(maxInputFrames, sr);

        backingStretch.presetDefault(2, (float) sr);
        backingStretch.reset();
        backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);

        backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
        backingBuffer.setSize(2, bs, false, false, true);
    }

    cachedBackingDuration.store(backingTransport->getLengthInSeconds());
    cachedBackingPosition.store(0.0);
    backingHeardPositionSec.store(0.0, std::memory_order_relaxed);

    // Reset the loudness leveler for the new song: clearing the cached sample
    // rate forces renderBackingBlockLocked() to re-prepare() it on the next
    // block, dropping the previous track's AGC gain + limiter state. Otherwise
    // the ~300 ms gain follower would carry over and briefly mis-level the start
    // of a much louder/quieter next song. Safe here — loadBackingTrack holds
    // backingLock, the same lock the render path runs under.
    backingLevelerSr = 0.0;
    std::cerr << "[AudioEngine] loadBackingTrack OK sr=" << readerSampleRate
              << " len=" << readerLengthInSamples
              << std::endl;
    return true;
}

void AudioEngine::setBackingPosition(double seconds)
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->setPosition(seconds);
        backingStretch.reset();
        // Read back the actual position; the transport may clamp (e.g. negative or past EOF).
        const double pos = backingTransport->getCurrentPosition();
        cachedBackingPosition.store(pos);
        backingHeardPositionSec.store(pos, std::memory_order_relaxed);
    }
}

void AudioEngine::startBacking()
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->start();
        backingPlaying.store(true);
        backingHeardPositionSec.store(backingTransport->getCurrentPosition(),
                                      std::memory_order_relaxed);
    }
}

void AudioEngine::stopBackingNoLock()
{
    if (backingTransport)
    {
        backingTransport->stop();
        backingStretch.reset();
        backingPlaying.store(false);
    }
    currentBackingLevel.store(0.0f);
}

void AudioEngine::stopBacking()
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
}

void AudioEngine::setBackingSpeed(double speed)
{
    if (!std::isfinite(speed) || speed <= 0.0)
    {
        return;
    }

    const double clamped = juce::jlimit(0.01, kMaxBackingSpeed, speed);
    // Dead-zone against the last *requested* rate to coalesce rapid slider
    // ticks — but never skip a change that crosses the 1× bypass boundary, or a
    // request just shy of 1× (e.g. 0.9995 -> 1.0, diff < 0.001) would leave the
    // stretcher path engaged when the caller actually asked for transparent
    // full speed.
    const double prev = backingPendingSpeed.load(std::memory_order_relaxed);
    const bool prevBypass = std::abs(prev    - 1.0) < kBackingSpeedBypassEpsilon;
    const bool newBypass  = std::abs(clamped - 1.0) < kBackingSpeedBypassEpsilon;
    if (std::abs(clamped - prev) < 0.001 && prevBypass == newBypass)
    {
        return;
    }

    // Lock-free hand-off to the audio thread. Publish the requested rate, then
    // raise the pending flag with release so the RT thread is guaranteed to see
    // the new rate once it observes the flag. renderBackingBlockLocked() adopts
    // the rate and resets the stretcher together, on the audio thread, so:
    //   * a control-thread caller (e.g. a speed slider at 30-60 Hz) never takes
    //     backingLock and so never starves the RT tryLock into dropping a block;
    //   * the new rate is never processed with stale stretch state — the reset
    //     and the rate adoption happen in the same RT block (see PR #237).
    // Multiple updates before the RT consumes them coalesce (latest wins), which
    // naturally throttles stretcher resets during a drag.
    backingPendingSpeed.store(clamped, std::memory_order_relaxed);
    backingSpeedChangePending.store(true, std::memory_order_release);
}

void AudioEngine::resetPeaks()
{
    // Input peak is per-source — clear EVERY active source (getSourceLevels() exposes
    // each one's peak), not just source 0, or extra-device peaks latch forever.
    for (auto& src : sources)
        if (src->isActive())
            src->resetInputPeak();
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
        const InputDeviceSlot& es = extraInputs[(size_t) (deviceKey - 1)];
        if (! es.active.load(std::memory_order_acquire) && es.desiredDeviceName.isEmpty())
            return -1;
    }

    std::lock_guard<std::mutex> lock(sourcesMutex);
    reclaimPendingReleases();  // free up any slot whose release was deferred

    // Find a free pooled slot (slot 0 is the permanent default). Skip a slot whose
    // release is still pending — its chain/worker hasn't been torn down yet, so
    // re-preparing it would double-start the verifier thread.
    int slot = -1;
    for (int i = 1; i < kMaxSources; ++i)
        if (! sources[(size_t) i]->isActive() && ! pendingRelease[(size_t) i]) { slot = i; break; }
    if (slot < 0)
        return -1;  // pool full

    SourceChain& src = *sources[(size_t) slot];
    src.setInputChannel(inputChannel);
    src.setDeviceKey(deviceKey);
    // Inherit the bound device's capture-latency correction (0 for the primary).
    // The device usually started before the source was created (the user picks the
    // device, then enables detect), so its delta is already known.
    if (deviceKey >= 1 && deviceKey <= kMaxExtraInputDevices)
        src.setVerifierAutoOffset(extraInputs[(size_t) (deviceKey - 1)].latencyDeltaSec.load(std::memory_order_relaxed));
    else
        src.setVerifierAutoOffset(0.0);
    // Clear any MANUAL offset left on this pooled chain by a previous player — a
    // freshly added source starts with no user fine-tune (the renderer re-applies
    // its own via setSourceVerifierOffset). releaseResources() doesn't touch it.
    src.setVerifierUserOffset(0.0);
    // Likewise clear stale meters so this source doesn't briefly report the previous
    // player's level/peak through getSourceLevels() until fresh audio arrives.
    src.resetInputMeters();

    // Prepare fully BEFORE making it visible to the audio thread, so the first
    // callback that observes active==true sees a ready chain + rings. When audio
    // isn't running yet, audioDeviceAboutToStart (primary) / extraInputAboutToStart
    // (extra) prepares it later. An EXTRA-device source must be prepared with ITS
    // device's sample rate / block size, not the primary's — the two can differ.
    bool deviceReady = audioRunning.load(std::memory_order_relaxed);
    double sr = currentSampleRate.load(std::memory_order_relaxed);
    int bs = inputBlockSize.load(std::memory_order_relaxed);
    if (deviceKey >= 1 && deviceKey <= kMaxExtraInputDevices)
    {
        const InputDeviceSlot& es = extraInputs[(size_t) (deviceKey - 1)];
        deviceReady = es.active.load(std::memory_order_acquire);
        sr = es.sampleRate.load(std::memory_order_relaxed);
        bs = es.blockSize.load(std::memory_order_relaxed);
    }
    if (deviceReady && sr > 0.0 && bs > 0)
        src.prepare(sr, bs);
    src.setActive(true);  // release-store: now picked up by the audio callback
    return slot;
}

bool AudioEngine::removeSource(int id)
{
    if (id <= 0 || id >= kMaxSources)
        return false;  // 0 is permanent; out-of-range rejected

    std::lock_guard<std::mutex> lock(sourcesMutex);
    reclaimPendingReleases();  // opportunistically reclaim earlier deferrals

    SourceChain& src = *sources[(size_t) id];
    if (! src.isActive())
        return false;

    // Hide it from the audio callback first; subsequent blocks snapshot active
    // once and skip it. It is logically removed from here on, regardless of when
    // its resources are reclaimed.
    src.setActive(false);

    // Reclaim now if we can confirm THIS SOURCE's device callback is not executing.
    // Only the callback for the source's own deviceKey can touch it; that counter is
    // decremented at the callback's real exit (release store), so observing 0
    // (acquire) proves it is not inside processBlock right now; any callback that
    // starts afterwards snapshots active and skips this (now-inactive) source — so
    // releasing cannot race the audio thread. Keying on the source's deviceKey (not a
    // global all-callbacks-idle check) is what lets removals reclaim during steady
    // multi-device playback, when callbacks on independent clocks are never all idle
    // at once. Bounded so a wedged device can't hang this thread.
    const size_t dk = (size_t) src.getDeviceKey();
    for (int spins = 0; spins < 200; ++spins)  // ~200 ms cap
    {
        if (callbacksInFlight[dk].load(std::memory_order_acquire) == 0)
        {
            src.releaseResources();  // stops its threads + releases its chain
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // A callback stayed wedged in-flight past the wait (a >200 ms block would be a
    // catastrophic stall). Do NOT force a release that could race it — DEFER it.
    // The source is already inactive so no future callback touches it; reclaim it
    // later (next add/removeSource, or audioDeviceStopped) when the body is quiet.
    pendingRelease[(size_t) id] = true;
    return true;
}

void AudioEngine::reclaimPendingReleases()
{
    // Caller holds sourcesMutex. A deferred source is inactive (future callbacks skip
    // it); releasing it is safe once the callback for ITS deviceKey is not in a body.
    // We key per-deviceKey (decremented by each callback at its real exit) so a
    // pending release frees as soon as its OWN device is quiescent — not only when
    // every device callback happens to be idle simultaneously (which, on independent
    // clocks during steady multi-device playback, may never occur and would strand
    // the slot until full stop). On the audioDeviceStopped path the relevant callback
    // already left its count at 0.
    for (int i = 1; i < kMaxSources; ++i)
    {
        if (! pendingRelease[(size_t) i]) continue;
        const size_t dk = (size_t) sources[(size_t) i]->getDeviceKey();
        if (callbacksInFlight[dk].load(std::memory_order_acquire) != 0)
            continue;  // this source's device is mid-body — try again later
        sources[(size_t) i]->releaseResources();
        pendingRelease[(size_t) i] = false;
    }
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
    if (id < 0 || id >= kMaxSources)
        return nullptr;
    SourceChain& src = *sources[(size_t) id];
    return (id == 0 || src.isActive()) ? &src : nullptr;
}

void AudioEngine::setMlNoteDetectionEnabled(bool e)
{
    // Fan to every source in the fixed pool (not just active ones) so a source
    // activated later inherits the current arm state instead of silently
    // staying dormant. Each MlNoteDetector::setEnabled is a cheap atomic + a
    // cold-state clear on a real transition.
    for (int i = 0; i < kMaxSources; ++i)
        sources[(size_t) i]->getMlNoteDetector().setEnabled(e);
}

std::vector<AudioEngine::SourceInfo> AudioEngine::listSources() const
{
    std::vector<SourceInfo> out;
    for (int i = 0; i < kMaxSources; ++i)
    {
        const SourceChain& src = *sources[(size_t) i];
        if (! src.isActive()) continue;
        SourceInfo info;
        info.id = src.getId();
        info.inputChannel = src.getInputChannel();
        info.deviceKey = src.getDeviceKey();
        info.active = true;
        out.push_back(info);
    }
    return out;
}

int AudioEngine::renderBackingBlockLocked(int numSamples)
{
    // Adopt any speed change requested since the last block (set lock-free by
    // setBackingSpeed). Common (no-change) path is a plain acquire load — no
    // locked RMW, so the flag's cache line stays shared and isn't bounced to
    // this core every callback. Only the rare block that actually consumes a
    // change does the exchange (clearing the flag atomically so a concurrent
    // setBackingSpeed can't lose an update). The acquire pairs with the
    // release-store in setBackingSpeed so the new rate is visible here. Reset
    // the stretcher and re-anchor the heard position in the SAME block we adopt
    // the rate, so a block is never processed at the new rate with stale stretch
    // state. reset() only clears state (no allocation), so it's audio-thread safe.
    if (backingSpeedChangePending.load(std::memory_order_acquire))
    {
        backingSpeedChangePending.exchange(false, std::memory_order_acquire);
        backingSpeed.store(juce::jlimit(0.01, kMaxBackingSpeed,
                                        backingPendingSpeed.load(std::memory_order_relaxed)),
                           std::memory_order_relaxed);
        backingStretch.reset();
        backingHeardPositionSec.store(backingTransport->getCurrentPosition(),
                                      std::memory_order_relaxed);
    }

    const double rate = juce::jlimit(0.01, kMaxBackingSpeed, backingSpeed.load(std::memory_order_relaxed));

    // Defensive clamp: the buffers are sized in audioDeviceAboutToStart() /
    // audioOutputAboutToStart() from the device's nominal block size, but a
    // callback can deliver a larger numSamples on a device-reconfig race. Drop
    // the excess frames silently rather than reading/writing past the allocated
    // span; the next callback after reconfig arrives at the new nominal size.
    const int outCap = backingBuffer.getNumSamples();
    const int inCap  = backingInputBuffer.getNumSamples();
    const int outSamples = juce::jmin(numSamples, outCap);
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    const bool bypassStretch = std::abs(rate - 1.0) < kBackingSpeedBypassEpsilon;

    int sourceFramesPulled = 0;

    if (bypassStretch)
    {
        // 1× — direct transport read, no phase-vocoder path. (The transport
        // still sample-rate-converts the file to the device rate, so this is
        // "no time-stretch", not necessarily bit-perfect.)
        backingBuffer.clear(0, outSamples);
        juce::AudioSourceChannelInfo info(&backingBuffer, 0, outSamples);
        backingTransport->getNextAudioBlock(info);
        sourceFramesPulled = outSamples;
    }
    else
    {
        // Slow/fast path — pull only the source frames needed for this output
        // block (output * rate), then stretch in-process to fill outSamples.
        const int inputFrames = juce::jmin((int) std::ceil(outSamples * rate), inCap);

        backingInputBuffer.clear(0, inputFrames);
        juce::AudioSourceChannelInfo info(&backingInputBuffer, 0, inputFrames);
        backingTransport->getNextAudioBlock(info);
        sourceFramesPulled = inputFrames;

        backingBuffer.clear(0, outSamples);

        const float* const* inPtrs  = backingInputBuffer.getArrayOfReadPointers();
        float* const* outPtrs = backingBuffer.getArrayOfWritePointers();
        backingStretch.process(inPtrs, inputFrames, outPtrs, outSamples);
    }

    const double transportPos = backingTransport->getCurrentPosition();
    if (sr > 0.0 && sourceFramesPulled > 0)
    {
        // Accumulate the heard (source) position, but clamp to the transport's
        // actual position. sourceFramesPulled is the requested block size; a
        // short read (e.g. at EOF, where the transport returns fewer real frames
        // and zero-pads) would otherwise advance the playhead past the true
        // source point and report progress beyond the track duration before
        // backingPlaying flips false. getCurrentPosition() stays clamped to the
        // real source position.
        double heard = backingHeardPositionSec.load(std::memory_order_relaxed)
                       + static_cast<double>(sourceFramesPulled) / sr;
        heard = juce::jmin(heard, transportPos);
        backingHeardPositionSec.store(heard, std::memory_order_relaxed);

        // Bypass reads straight from the transport — no phase-vocoder output
        // latency to compensate for. Only the stretch path adds latency.
        const double latencyInputSec = bypassStretch
            ? 0.0
            : (backingStretchLatencySamples.load(std::memory_order_relaxed) * rate) / sr;
        cachedBackingPosition.store(juce::jmax(0.0, heard - latencyInputSec));
    }
    else
    {
        // currentSampleRate is transiently 0 during device teardown/reconfig.
        // We can't accumulate (no Hz to divide by), so anchor both the heard
        // accumulator and the published playhead to the real transport position
        // rather than leaving a stale value visible to the UI.
        backingHeardPositionSec.store(transportPos, std::memory_order_relaxed);
        cachedBackingPosition.store(juce::jmax(0.0, transportPos));
    }

    // Sync the flag if transport stopped at EOF.
    if (!backingTransport->isPlaying())
        backingPlaying.store(false);

    // Normalize the backing track to a consistent target loudness (-12 LUFS)
    // BEFORE the mixer's backing-volume fader is applied (later in the RT
    // callback), so every song sits at the same level while the fader still
    // attenuates it. Standard BS.1770 K-weighting (full-mix music) + a brickwall
    // limiter to keep boosted peaks safe. RT-safe (no allocation).
    if (outSamples > 0 && sr > 0.0)
    {
        if (sr != backingLevelerSr) { backingLeveler.prepare(sr); backingLevelerSr = sr; }
        backingLeveler.process(backingBuffer, outSamples, -12.0f);
    }

    return outSamples;
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
    if (streamMixScratch.getNumSamples() < streamScratchCap)    streamMixScratch.setSize(2, streamScratchCap, false, false, true);

    // Prepare each ACTIVE PRIMARY-device source's DSP and reset its rings for a
    // clean cold start. Inactive pooled chains stay unprepared (no threads). EXTRA-
    // device sources (deviceKey > 0) are prepared by their own extraInputAboutToStart
    // with THAT device's format — a primary restart must not clobber them with the
    // primary's sample rate / block size (they run on a different hardware clock).
    for (auto& src : sources)
        if (src->isActive() && src->getDeviceKey() == 0)
            src->prepare(sr, bs);

    // Split mode preps the backing stretcher in audioOutputAboutToStart
    // instead — that callback owns the device the backing audio actually
    // plays on, and pulls from backingTransport at the output device's
    // block size.
    if (duplexMode.load(std::memory_order_relaxed))
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport)
        {
            // See loadBackingTrack() for why prepareToPlay uses maxInputFrames
            // rather than bs: the RT callback can pull ceil(bs * kMaxBackingSpeed)
            // frames in a single block at faster-than-1× speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioDeviceStopped()
{
    // JUCE calls audioDeviceStopped() only AFTER the PRIMARY input device has
    // stopped invoking its IO callback (stop() blocks for the callback thread to
    // finish), so the primary body is quiescent here. We release ONLY deviceKey-0
    // sources: an EXTRA-device source is processed by that device's own callback,
    // which may still be running on its own thread — releasing it here would race.
    // Extra sources are released by extraInputStopped()/unbindInputDevice().
    audioRunning.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        // Release each ACTIVE primary-device source's chain and zero its rings.
        // Inactive pooled chains were never prepared.
        for (auto& src : sources)
            if (src->isActive() && src->getDeviceKey() == 0)
                src->releaseResources();
        // Reclaim deferred removals only when ALL callback bodies are quiescent
        // (an extra-device callback could still be mid-block).
        reclaimPendingReleases();
    }
    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    currentBackingLevel.store(0.0f);

    // Note on split-mode lifecycle: we deliberately do NOT detach the
    // output callback here. JUCE auto-restarts a transiently-stopped input
    // device by re-firing audioDeviceAboutToStart() on its own; that path
    // doesn't re-add the output callback, so detaching would break
    // automatic recovery (output stays silent until a manual reconfigure).
    // While input is down, the guitar/DSP side of the output goes silent
    // (no producer feeding outputPendingRing, so the consumer's underflow
    // branch zero-fills), but the backing track keeps playing — the output
    // callback mixes backingTransport independently of ring state. That's
    // intentional UX: a user unplugging their interface mid-song doesn't
    // lose their place. Real teardown belongs to stopAudio() / teardownSplitMode().
}

void AudioEngine::audioOutputAboutToStart(juce::AudioIODevice* device)
{
    const int bs = device->getCurrentBufferSizeSamples();
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
    if (streamMixScratch.getNumSamples() < streamScratchCap)    streamMixScratch.setSize(2, streamScratchCap, false, false, true);
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
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport && sr > 0.0 && bs > 0)
        {
            // Mirror loadBackingTrack() / audioDeviceAboutToStart() — the
            // output device drives backing playback in split mode, so this
            // is where the stretcher gets sized for that side. prepareToPlay
            // upper-bounds future getNextAudioBlock requests, and the
            // RT callback can pull ceil(bs * kMaxBackingSpeed) at faster
            // speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioOutputStopped()
{
    // No-op by design. The consumer's catch-up branch in audioOutputCallback
    // handles both (w - r) > cap (producer lapped during the stop) and
    // w < r (a future reset race) on the next output start, so we don't
    // need to reset readIndex here. Resetting r to 0 while the producer
    // keeps advancing w is equivalent — both end up in the catch-up branch
    // — but it leaves a transient window where r appears reset without the
    // ring invariants being re-established, which is harder to reason about.
}

// ── Streamer mix output sink (PR1) ──────────────────────────────────────────
// Producer (primary/output callback): compose the stream submix and pack it into
// the sink's ring. Consumer (streamSinkCallback): drain + write to the device.
// Mirrors the InputDeviceSlot ring discipline, inverted to the output side.

void AudioEngine::composeAndPushStreamMix(const juce::AudioBuffer<float>& guitarMix,
                                          const juce::AudioBuffer<float>* backingBuf,
                                          int backingFrames, float backingVol, int numSamples)
{
    if (! streamSink.active.load(std::memory_order_acquire)) return;
    // A block larger than the entire ring can't be published atomically (it would
    // wrap and overwrite unread slots before writeIndex is bumped). Skip it and
    // count an overflow. Checked FIRST (before the scratch guard) so an oversized
    // block is always counted — the fixed-size scratch is exactly the ring, so an
    // oversized block also trips the scratch guard below and would otherwise be
    // dropped silently. The split path already rejects oversized devices at setup;
    // this guards the duplex path, whose block size we don't pre-validate.
    if (numSamples > (int) kOutputRingFrames)
    {
        streamSink.overflowCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Scratch not yet sized to the full ring (cold start before the producer's
    // about-to-start ran, or a transient reconfig) — skip rather than alloc on RT.
    // After warm-up the scratch is exactly the ring, so for an in-range block this
    // never trips.
    if (streamMixScratch.getNumSamples() < numSamples) return;

    const bool  ig   = streamBusIncludeGuitar.load(std::memory_order_relaxed);
    const bool  ib   = streamBusIncludeBacking.load(std::memory_order_relaxed);
    const float gain = streamBusGain.load(std::memory_order_relaxed);

    streamMixScratch.clear(0, 0, numSamples);
    streamMixScratch.clear(1, 0, numSamples);
    if (ig)
        for (int ch = 0; ch < 2; ++ch)
            streamMixScratch.addFrom(ch, 0, guitarMix,
                juce::jmin(ch, guitarMix.getNumChannels() - 1), 0, numSamples);
    if (ib && backingBuf != nullptr && backingFrames > 0)
    {
        const int n = juce::jmin(backingFrames, numSamples);
        for (int ch = 0; ch < 2; ++ch)
            streamMixScratch.addFrom(ch, 0, *backingBuf,
                juce::jmin(ch, backingBuf->getNumChannels() - 1), 0, n, backingVol);
    }
    streamMixScratch.applyGain(0, 0, numSamples, gain);
    streamMixScratch.applyGain(1, 0, numSamples, gain);

    const float peak = juce::jmax(streamMixScratch.getMagnitude(0, 0, numSamples),
                                  streamMixScratch.getMagnitude(1, 0, numSamples));
    streamSinkLevel.store(peak, std::memory_order_relaxed);

    packStereoIntoRing(streamMixScratch, numSamples, streamSink.ring, streamSink.writeIndex);
}

void AudioEngine::streamSinkCallback(float* const* outputData, int numOutputChannels, int numSamples)
{
    const juce::ScopedNoDenormals noDenormals;
    if (numOutputChannels <= 0) return;
    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);
    buffer.clear();

    constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
    constexpr uint64_t kCap  = (uint64_t) kOutputRingFrames;
    const int scratchCap = (int) streamSink.pullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = streamSink.readIndex.load(std::memory_order_relaxed);
    const uint64_t w = streamSink.writeIndex.load(std::memory_order_acquire);
    if (w < r) { r = w; streamSink.readIndex.store(r, std::memory_order_relaxed); }
    if ((w - r) > kCap)
    {
        r = w - kCap;
        streamSink.readIndex.store(r, std::memory_order_relaxed);
        streamSink.overflowCount.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t available   = w - r;
    const int      pullCount   = juce::jmin(outSamples, (int) available);
    const int      consumeCount = juce::jmin(numSamples, (int) available);
    const int      copyChannels = juce::jmin(numOutputChannels, 2);
    for (int i = 0; i < pullCount; ++i)
    {
        const uint64_t slot = (r + (uint64_t) i) & kMask;
        float l, rr;
        unpackLR(streamSink.ring[(size_t) slot].load(std::memory_order_relaxed), l, rr);
        buffer.setSample(0, i, l);
        if (copyChannels > 1) buffer.setSample(1, i, rr);
    }
    if (pullCount < outSamples)
        streamSink.underflowCount.fetch_add(1, std::memory_order_relaxed);
    streamSink.readIndex.store(r + (uint64_t) consumeCount, std::memory_order_release);
}

void AudioEngine::streamSinkAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr) return;
    const int bs = device->getCurrentBufferSizeSamples();
    double sr = device->getCurrentSampleRate();
    if (sr <= 0.0) sr = currentSampleRate.load(std::memory_order_relaxed);
    streamSink.blockSize.store(bs, std::memory_order_relaxed);
    streamSink.sampleRate.store(sr, std::memory_order_relaxed);
    const int cap = juce::jmax(bs, 2048);
    if ((int) streamSink.pullScratchL.size() < cap) streamSink.pullScratchL.assign((size_t) cap, 0.0f);
    if ((int) streamSink.pullScratchR.size() < cap) streamSink.pullScratchR.assign((size_t) cap, 0.0f);
    streamSink.writeIndex.store(0, std::memory_order_relaxed);
    streamSink.readIndex.store(0, std::memory_order_relaxed);
    streamSink.underflowCount.store(0, std::memory_order_relaxed);
    streamSink.overflowCount.store(0, std::memory_order_relaxed);
    for (auto& v : streamSink.ring) v.store(0, std::memory_order_relaxed);
}

void AudioEngine::streamSinkStopped()
{
    // Fires on any stop of the stream device — an unplanned loss (unplug / driver
    // reset) as well as our own teardown. Mark inactive so the producer stops
    // filling a now-consumer-less ring and the meter clears; desiredTypeName/Name
    // are deliberately left intact so reopenDesiredStreamSink() can restore it.
    streamSink.active.store(false, std::memory_order_release);
    streamSinkLevel.store(0.0f, std::memory_order_relaxed);
}

juce::String AudioEngine::setStreamOutputDevice(const juce::String& typeName, const juce::String& deviceName)
{
    // Control-thread only. Opens an OUTPUT-only device on the stream sink's own
    // AudioDeviceManager and attaches the drain callback. Mirrors applySplitSetup's
    // output open. v1 requires the sink's nominal SR to match the engine rate (no
    // async resampler yet — that's PR3); a mismatch is rejected with a clear error.
    streamSink.desiredTypeName = typeName;
    streamSink.desiredDeviceName = deviceName;

    // Stop the producer from writing the ring while we (re)configure the device:
    // setAudioDeviceSetup() below drives streamSinkAboutToStart(), which resets the
    // ring indices and slots. Clearing `active` first stops the main callback from
    // STARTING new pushes. A producer block already past the active-check can still
    // finish one push, but the device close/reopen takes far longer than a single
    // audio block, so that in-flight push completes well before about-to-start runs.
    // Worst case is therefore one imperfect block on the STREAM bus (never the local
    // monitor) during a manual device switch — atomic, no data race, no UAF. We only
    // re-arm `active` after a fully clean open.
    streamSink.active.store(false, std::memory_order_release);

    // Any failure below: detach/close the half-open device AND drop the desired
    // intent. A deterministic failure (e.g. SR mismatch) is then NOT retried on every
    // startAudio(), and the engine never reports active with no device behind it. The
    // renderer keeps its own persisted choice and re-applies it, so nothing is lost.
    auto fail = [this](const juce::String& msg) -> juce::String {
        closeStreamSinkDevice();
        streamSink.desiredTypeName = {};
        streamSink.desiredDeviceName = {};
        return msg;
    };

    if (! streamSink.initialised)
    {
        streamSink.manager.initialise(0, 2, nullptr, false);
        streamSink.initialised = true;
    }

    juce::AudioIODeviceType* outType = nullptr;
    for (auto* t : streamSink.manager.getAvailableDeviceTypes())
        if (t->getTypeName() == typeName) { outType = t; break; }
    if (! outType) return fail("Stream output device type not found: " + typeName);

    try {
        if (auto* cur = streamSink.manager.getCurrentDeviceTypeObject())
        {
            if (cur->getTypeName() != typeName)
                streamSink.manager.setCurrentAudioDeviceType(typeName, true);
        }
        else streamSink.manager.setCurrentAudioDeviceType(typeName, true);
    } catch (...) { return fail("setCurrentAudioDeviceType threw for stream output type '" + typeName + "'"); }

    juce::String resolved = deviceName;
    if (resolved.isEmpty())
    {
        auto names = outType->getDeviceNames(false);
        if (names.size() > 0) resolved = names[0];
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName  = "";
    setup.outputDeviceName = resolved;
    setup.sampleRate = currentSampleRate.load(std::memory_order_relaxed);
    setup.bufferSize = outputBlockSize.load(std::memory_order_relaxed);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.setRange(0, 2, true);

    juce::String err;
    try { err = streamSink.manager.setAudioDeviceSetup(setup, true); }
    catch (...) { return fail("stream output setAudioDeviceSetup threw"); }
    if (err.isNotEmpty()) return fail("stream output setup: " + err);

    auto* dev = streamSink.manager.getCurrentAudioDevice();
    if (! dev) return fail("no stream output device after setup");
    const double devSr = dev->getCurrentSampleRate();
    const double engineSr = currentSampleRate.load(std::memory_order_relaxed);
    if (engineSr > 0.0 && std::abs(devSr - engineSr) > 0.5)
        return fail("Stream output sample rate (" + juce::String(devSr)
             + ") must match the engine rate (" + juce::String(engineSr)
             + "). Pick a device that supports " + juce::String(engineSr) + " Hz.");

    streamSink.callback.engine = this;
    if (! streamSink.callbackRegistered)
    {
        streamSink.manager.addAudioCallback(&streamSink.callback);
        streamSink.callbackRegistered = true;
    }
    streamSink.active.store(true, std::memory_order_release);
    fprintf(stderr, "[AudioEngine] stream output active: %s (%s)\n",
            resolved.toRawUTF8(), typeName.toRawUTF8());
    return {};
}

void AudioEngine::closeStreamSinkDevice()
{
    // Detach the drain callback and close the device, leaving desiredTypeName/Name
    // intact so startAudio()/reopenDesiredStreamSink() can restore it. active=false
    // first so the producer stops pushing; removeAudioCallback() then blocks until
    // the consumer callback is no longer in flight, so the ring/device go quiescent
    // before close. Idempotent — safe when nothing is open.
    streamSink.active.store(false, std::memory_order_release);
    if (streamSink.callbackRegistered)
    {
        streamSink.manager.removeAudioCallback(&streamSink.callback);
        streamSink.callbackRegistered = false;
    }
    try { streamSink.manager.closeAudioDevice(); } catch (...) {}
    streamSinkLevel.store(0.0f, std::memory_order_relaxed);
}

void AudioEngine::clearStreamOutput()
{
    closeStreamSinkDevice();
    streamSink.desiredTypeName = {};
    streamSink.desiredDeviceName = {};
}

void AudioEngine::reopenDesiredStreamSink()
{
    // Copy the intent first: setStreamOutputDevice() mutates desiredTypeName/Name
    // (and clears them on failure), so don't pass the members in by reference.
    const juce::String t = streamSink.desiredTypeName;
    const juce::String d = streamSink.desiredDeviceName;
    if (d.isEmpty() && t.isEmpty()) return;
    const juce::String err = setStreamOutputDevice(t, d);
    if (err.isNotEmpty())
        fprintf(stderr, "[AudioEngine] reopenDesiredStreamSink failed: %s\n", err.toRawUTF8());
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
    callbacksInFlight[0].fetch_add(1, std::memory_order_acq_rel);

    const bool duplex = duplexMode.load(std::memory_order_relaxed);

    // Duplex writes outputData directly. Split runs DSP into a private 2-channel
    // scratch and pushes the result to outputPendingRing for OutputCallback.
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
    mixSourcesForDevice(0, inputData, numInputChannels, buffer, sourceMonitorScratch,
                        effectiveOutputChannels, numSamples);

    // Duplex mixes backing + applies output gain + meters here.
    // Split defers all three to OutputCallback (output device's clock).
    if (duplex)
    {
        // Stream sink (producer): snapshot the guitar monitor mix BEFORE backing is
        // added, so the stream submix can carry guitar independent of the local mix.
        const bool streamActive = streamSink.active.load(std::memory_order_acquire);
        int   streamBackingFrames = 0;
        float streamBackingVol    = 0.0f;
        bool  streamBackingOn     = false;
        if (streamActive && streamGuitarScratch.getNumSamples() >= numSamples)
            for (int ch = 0; ch < 2; ++ch)
                streamGuitarScratch.copyFrom(ch, 0, buffer,
                    juce::jmin(ch, buffer.getNumChannels() - 1), 0, numSamples);

        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            const int outSamples = renderBackingBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            streamBackingFrames = outSamples; streamBackingVol = bVol; streamBackingOn = true;
            const int mixChannels = juce::jmin(numOutputChannels, 2);
            float backingLevelSq = 0.0f;
            for (int ch = 0; ch < mixChannels; ++ch)
            {
                const float* const src = backingBuffer.getReadPointer(ch);
                float sumSquares = 0.0f;
                for (int i = 0; i < outSamples; ++i)
                    sumSquares += src[i] * src[i];

                const float channelRms = (outSamples > 0)
                    ? std::sqrt(sumSquares / outSamples) * bVol
                    : 0.0f;
                backingLevelSq += channelRms * channelRms;
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, outSamples, bVol);
            }
            currentBackingLevel.store((mixChannels > 0)
                ? std::sqrt(backingLevelSq / mixChannels)
                : 0.0f);
        }
        else
        {
            currentBackingLevel.store(0.0f);
        }

        // Stream sink: compose + push the stream submix (guitar snapshot + backing)
        // BEFORE the local master output gain, so the stream level is independent.
        if (streamActive)
            composeAndPushStreamMix(streamGuitarScratch,
                                    streamBackingOn ? &backingBuffer : nullptr,
                                    streamBackingFrames, streamBackingVol, numSamples);

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
        packStereoIntoRing(buffer, numSamples, outputPendingRing, outputRingWriteIndex);
    }

    // Body done — this callback is no longer processing a source. Pairs with
    // removeSource()/reclaimPendingReleases() acquire loads. Index 0 = primary.
    callbacksInFlight[0].fetch_sub(1, std::memory_order_acq_rel);
}

int AudioEngine::mixSourcesForDevice(int deviceKey, const float* const* inputData, int numInputChannels,
                                     juce::AudioBuffer<float>& mixBuf, juce::AudioBuffer<float>& monitorScratch,
                                     int effectiveOutputChannels, int numSamples)
{
    // Snapshot each source's active flag ONCE so the count and the process/mix
    // passes are consistent within this block — a concurrent add/removeSource
    // flipping a flag between two reads must not change which branch runs.
    // removeSource waits for all callback bodies to drain before releasing, so a
    // source snapshotted active here is safe even if deactivated an instant later.
    bool act[kMaxSources];
    int firstActive = -1, activeCount = 0;
    for (int i = 0; i < kMaxSources; ++i)
    {
        act[i] = sources[(size_t) i]->isActive()
                 && sources[(size_t) i]->getDeviceKey() == deviceKey;
        if (act[i]) { ++activeCount; if (firstActive < 0) firstActive = i; }
    }

    if (activeCount == 0)
    {
        // No source on this device → silence. An extra device with no bound source
        // contributes nothing to the output sum. The primary always has sources[0]
        // (deviceKey 0, active from construction), so it never reaches this branch.
        for (int ch = 0; ch < effectiveOutputChannels; ++ch)
            mixBuf.clear(ch, 0, numSamples);
        return 0;
    }

    if (activeCount == 1)
    {
        // Fast path — exactly one source: process in place on mixBuf, byte-
        // identical to the single-pipeline engine (channel select / mono mix +
        // input gain, metering, ML + ring feed, gate, YIN, tone chain, monitor).
        sources[(size_t) firstActive]
            ->processBlock(inputData, numInputChannels, mixBuf, effectiveOutputChannels, numSamples);
        return 1;
    }

    // Multi-source: each renders its own 2-channel monitor into monitorScratch
    // (each builds its mono from its bound channel + feeds its own rings /
    // detectors / verifier), summed to STEREO (0/1). A >2-channel output keeps
    // channels 2+ silent in multi-source mode (the fast path still broadcasts).
    for (int ch = 0; ch < effectiveOutputChannels; ++ch)
        mixBuf.clear(ch, 0, numSamples);
    const int mixCh = juce::jmin(effectiveOutputChannels, 2);
    const int n = juce::jmin(numSamples, monitorScratch.getNumSamples());
    for (int i = 0; i < kMaxSources; ++i)
    {
        if (! act[i]) continue;
        sources[(size_t) i]->processBlock(inputData, numInputChannels, monitorScratch, 2, n);
        for (int ch = 0; ch < mixCh; ++ch)
            mixBuf.addFrom(ch, 0, monitorScratch, ch, 0, n);
    }
    return activeCount;
}

void AudioEngine::packStereoIntoRing(const juce::AudioBuffer<float>& buf, int numSamples,
                                     std::array<std::atomic<uint64_t>, kOutputRingFrames>& ring,
                                     std::atomic<uint64_t>& writeIndex)
{
    // Strict SPSC: the producer (one device callback) only ever writes writeIndex;
    // the consumer (audioOutputCallback) is the sole writer of the paired
    // readIndex. Drop-oldest = letting writeIndex lap the buffer; the consumer
    // advances readIndex when it observes (w - r) > cap. The single packed store
    // prevents an L/R tear when the producer wraps mid-callback (relaxed because
    // ordering is established by the release on writeIndex below).
    constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
    const uint64_t w = writeIndex.load(std::memory_order_relaxed);
    const float* L = buf.getReadPointer(0);
    const float* R = buf.getReadPointer(1);
    for (int i = 0; i < numSamples; ++i)
    {
        const uint64_t slot = (w + (uint64_t) i) & kMask;
        ring[slot].store(packLR(L[i], R[i]), std::memory_order_relaxed);
    }
    writeIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
}

void AudioEngine::extraInputCallback(int slot, const float* const* inputData, int numInputChannels, int numSamples)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices) return;
    InputDeviceSlot& s = extraInputs[(size_t) slot];
    if (! s.active.load(std::memory_order_acquire)) return;

    callbacksInFlight[(size_t) s.deviceKey].fetch_add(1, std::memory_order_acq_rel);

    // Clamp to the per-slot scratch sized in extraInputAboutToStart so the hot
    // loop never allocates if a reconfig race delivers a larger block.
    const int cap = s.fanScratch.getNumSamples();
    if (numSamples > cap) numSamples = cap;

    juce::AudioBuffer<float> mix;
    mix.setDataToReferTo(s.fanScratch.getArrayOfWritePointers(), 2, numSamples);
    mixSourcesForDevice(s.deviceKey, inputData, numInputChannels, mix, s.monitorScratch, 2, numSamples);
    packStereoIntoRing(mix, numSamples, s.ring, s.writeIndex);

    callbacksInFlight[(size_t) s.deviceKey].fetch_sub(1, std::memory_order_acq_rel);
}

void AudioEngine::extraInputAboutToStart(int slot, juce::AudioIODevice* device)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices || device == nullptr) return;
    InputDeviceSlot& s = extraInputs[(size_t) slot];
    const int bs = device->getCurrentBufferSizeSamples();
    s.blockSize.store(bs, std::memory_order_relaxed);
    // Prepare against this DEVICE's actual sample rate — the source of truth.
    // bindInputDevice forces it to (and verifies it equals) the engine rate, so the
    // verifier (which reads the engine-wide currentSampleRate) and the detectors
    // agree. Reading the device here rather than assuming currentSampleRate keeps
    // the prepare correct even if a future path opens it differently.
    double sr = device->getCurrentSampleRate();
    if (sr <= 0.0) sr = currentSampleRate.load(std::memory_order_relaxed);
    s.sampleRate.store(sr, std::memory_order_relaxed);
    // Size per-slot scratch generously (cold-start guard) on this device-management
    // thread — never the RT thread.
    const int cap = juce::jmax(bs, 2048);
    s.fanScratch.setSize(2, cap, false, false, true);
    s.monitorScratch.setSize(2, cap, false, false, true);
    s.fanScratch.clear();
    s.monitorScratch.clear();
    s.writeIndex.store(0, std::memory_order_relaxed);
    s.readIndex.store(0, std::memory_order_relaxed);
    for (auto& v : s.ring) v.store(0, std::memory_order_relaxed);

    // Capture-latency correction: the renderer's playhead is aligned to the PRIMARY
    // device's input latency, but this extra device captures with a different
    // latency, so its audio sits at a different song-time than the playhead assumes.
    // Set its sources' verifier offset to (extra − primary) input latency so they
    // match this device's just-captured audio against the right chart notes.
    int extraLatSamples = device->getInputLatencyInSamples();
    int primaryLatSamples = 0;
    if (auto* pdev = inputDeviceManager.getCurrentAudioDevice())
        primaryLatSamples = pdev->getInputLatencyInSamples();
    // (extra − primary) reported input latency. On JACK/PipeWire this is 0 (no
    // latency reported); the residual per-device offset is instead dialed in by the
    // user via setSourceVerifierOffset (a stable auto-measure isn't possible — the
    // value is device-specific and signal-level-confounded). 0 here = no auto shift.
    const double deltaSec = (sr > 0.0) ? (double) (extraLatSamples - primaryLatSamples) / sr : 0.0;
    s.latencyDeltaSec.store(deltaSec, std::memory_order_relaxed);

    // Prepare each source bound to this device so its verifier/detectors run, and
    // apply the latency correction.
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        for (auto& src : sources)
            if (src->isActive() && src->getDeviceKey() == s.deviceKey)
            {
                src->prepare(sr, bs);
                src->setVerifierAutoOffset(deltaSec);
            }
    }
    s.active.store(true, std::memory_order_release);
}

void AudioEngine::extraInputStopped(int slot)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices) return;
    InputDeviceSlot& s = extraInputs[(size_t) slot];
    // JUCE blocks for this slot's callback thread before firing this, so the
    // slot's body is quiescent. Hide it from the output sum, then release ITS
    // sources (no other callback touches them — they all filter by deviceKey).
    s.active.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        // PERMANENT unbind (the user removed this device): the slot will never
        // re-open, so DEACTIVATE its sources too — leaving them "active" would strand
        // pooled slots no callback can ever service (a ghost detector in listSources).
        // A TRANSIENT close (stopAudio/reconfigure/unplug) only releases them, so
        // startAudio()'s re-open resumes them in place. Read the atomic flag (set by
        // the control-thread unbind) rather than the juce::String desiredDeviceName,
        // which this device-thread path must not race on.
        const bool permanent = s.permanentUnbind.load(std::memory_order_acquire);
        for (auto& src : sources)
            if (src->isActive() && src->getDeviceKey() == s.deviceKey)
            {
                src->releaseResources();
                // Zero the meters so getSourceLevels() reports silence while the
                // device is gone — otherwise the renderer's per-source silence gate
                // treats a stopped/unplugged input as still hearing audio (the last
                // non-zero level latches). releaseResources() doesn't touch them.
                src->resetInputMeters();
                if (permanent) src->setActive(false);
            }
        // Retry any removeSource() cleanup that was deferred waiting for callbacks to
        // drain. With this slot's callback now stopped, callbacksInFlight may finally
        // be 0; audioDeviceStopped() (primary) might never observe that window during
        // multi-device playback, so reclaim here too or a pending slot can leak until
        // the next add/remove.
        reclaimPendingReleases();
    }
    s.writeIndex.store(0, std::memory_order_relaxed);
    s.readIndex.store(0, std::memory_order_relaxed);
}

int AudioEngine::activeExtraInputCount() const
{
    int n = 0;
    for (const auto& s : extraInputs)
        if (s.active.load(std::memory_order_acquire)) ++n;
    return n;
}

juce::String AudioEngine::bindInputDevice(int deviceKey, const juce::String& deviceName)
{
    if (deviceKey < 1 || deviceKey > kMaxExtraInputDevices)
        return "deviceKey out of range";
    const int slot = deviceKey - 1;
    InputDeviceSlot& s = extraInputs[(size_t) slot];
    if (s.active.load(std::memory_order_acquire))
        return "device slot already bound";

    // Reject binding the SAME physical device into a second slot. Two callbacks
    // reading one interface is wasteful (and fails outright on exclusive drivers);
    // multiple sources that want this device should share its one deviceKey and pick
    // different channels instead. Checks both open + deferred (desired) slots.
    for (int other = 0; other < kMaxExtraInputDevices; ++other)
        if (other != slot && extraInputs[(size_t) other].desiredDeviceName == deviceName)
            return "device already bound to another input slot";

    // Reject binding the device that is the PRIMARY input — it is already "Main", and
    // opening it on this slot's manager too would double-open one interface on two
    // managers (fatal on exclusive backends). Critically this also guards the REOPEN
    // path: if the user makes a bound extra device the new main input, the preserved
    // intent must NOT resurrect it as an extra (reopenDesiredExtraInputs() then drops
    // the now-invalid binding via its failure handling).
    if (auto* primary = inputDeviceManager.getCurrentAudioDevice())
        if (primary->getName() == deviceName)
            return "device is the primary input — use Main, not an extra slot";

    // An extra input device requires SPLIT mode: the output callback owns the mix +
    // backing + gain and sums every device ring. In DUPLEX the primary device owns
    // both directions and the output manager is closed, so we cannot just flip the
    // flag — that would leave the output mix path absent (silent / unrouted). Reject
    // here so the renderer reconfigures to a separate output device first. Checked
    // BEFORE the deferred path below — startAudio()'s reopen also skips duplex, so a
    // deferred bind in duplex would silently never come up while reporting success.
    if (duplexMode.load(std::memory_order_relaxed))
        return "extra input requires split mode — select a separate output device first";

    // Deregister any STALE callback BEFORE touching the manager. An earlier unplanned
    // stop (USB unplug / backend restart) leaves s.callback registered; if we opened
    // the manager (initialise / setAudioDeviceSetup) with it still attached, JUCE
    // could dispatch it on the default/new device mid-setup — processing the wrong
    // hardware, or even firing extraInputAboutToStart() during a stopped-engine
    // validation open. Idempotent no-op when not registered.
    s.manager.removeAudioCallback(&s.callback);

    // Open `deviceName` input-only on this slot's own manager. initialise first so
    // the manager has a device type, then switch to the requested input device with
    // all its channels (the source picks a channel within).
    s.manager.initialiseWithDefaultDevices(2, 0);

    // The device name may belong to a device TYPE (ALSA / JACK / CoreAudio / …)
    // different from the slot manager's default — a JACK device name won't resolve
    // under ALSA and vice-versa ("No such device"). Find the type that actually
    // lists this input device and switch the slot manager to it. Prefer the primary
    // manager's current type (the devices the user already sees working).
    juce::String chosenType;
    if (auto* pt = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        pt->scanForDevices();
        if (pt->getDeviceNames(true).contains(deviceName))
            chosenType = pt->getTypeName();
    }
    if (chosenType.isEmpty())
        for (auto* t : s.manager.getAvailableDeviceTypes())
        {
            t->scanForDevices();
            if (t->getDeviceNames(true).contains(deviceName)) { chosenType = t->getTypeName(); break; }
        }
    // setCurrentAudioDeviceType can THROW from inside some JUCE backends (ASIO, and
    // misconfigured JACK/CoreAudio) — setAudioDevices() guards it for the primary, so
    // this path must too, or a bad backend terminates the process instead of
    // returning an error to the renderer. Close the slot manager on failure.
    if (chosenType.isNotEmpty())
    {
        try { s.manager.setCurrentAudioDeviceType(chosenType, true); }
        catch (...) { s.manager.closeAudioDevice(); return "extra-input setCurrentAudioDeviceType threw"; }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    s.manager.getAudioDeviceSetup(setup);
    setup.inputDeviceName = deviceName;
    setup.outputDeviceName = "";
    // Open ALL of the device's capture channels (not just the default first pair),
    // so a source bound to channel 2+ of a multi-channel extra interface actually
    // receives audio — mirrors the primary device's explicit full-range open.
    int inputChannelCount = 0;
    if (auto* t = s.manager.getCurrentDeviceTypeObject())
    {
        std::unique_ptr<juce::AudioIODevice> probe(t->createDevice({}, deviceName));
        if (probe) inputChannelCount = probe->getInputChannelNames().size();
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    setup.inputChannels.setRange(0, inputChannelCount, true);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    // Force the extra device to the ENGINE's sample rate. Each SourceChain's
    // verifier/detectors read the engine-wide currentSampleRate (bound by
    // reference at construction), so an extra input running at a different rate
    // (e.g. a 44.1 kHz device in a 48 kHz engine) would be scored on the wrong
    // clock — skewing pitch/timing for every source bound to it. Matching the
    // engine rate here (the OS/driver resamples if needed) keeps them coherent; a
    // device that cannot do this rate fails the setup below and is rejected.
    const double engineSr = currentSampleRate.load(std::memory_order_relaxed);
    if (engineSr > 0.0)
        setup.sampleRate = engineSr;
    // initialiseWithDefaultDevices above may have opened a default capture device on
    // this slot manager; every failure path below must close it, or a failed bind
    // leaves the interface captured until engine teardown (fatal on exclusive
    // backends + breaks retries / other apps).
    juce::String err;
    try { err = s.manager.setAudioDeviceSetup(setup, true); }
    catch (...) { s.manager.closeAudioDevice(); return "extra-input setAudioDeviceSetup threw"; }
    if (err.isNotEmpty())
    {
        s.manager.closeAudioDevice();
        return "extra input: " + err + (chosenType.isEmpty() ? " (no type lists this device)" : " (type " + chosenType + ")");
    }
    auto* extraDev = s.manager.getCurrentAudioDevice();
    if (extraDev == nullptr)
    {
        s.manager.closeAudioDevice();
        return "extra input device did not open";
    }

    // Some backends accept the rate request but actually open at a different rate.
    // Since the SourceChain verifier reads the engine-wide currentSampleRate, a
    // mismatch would score this device on the wrong clock — reject rather than
    // ship silently-wrong timing. (Tolerant of a sub-Hz rounding difference.)
    if (engineSr > 0.0 && std::abs(extraDev->getCurrentSampleRate() - engineSr) > 1.0)
    {
        const juce::String got = juce::String(extraDev->getCurrentSampleRate());
        s.manager.closeAudioDevice();
        return "extra input opened at " + got + " Hz, not the engine rate " + juce::String(engineSr) + " Hz";
    }

    // The device opened + validated. Record the INTENT now (not before the fallible
    // open above), so it drives re-open across a reconfigure without lingering after
    // a failed attach. Clear the permanent-unbind flag: a future stop on this slot is
    // transient (resume) until the user explicitly unbinds again.
    s.desiredDeviceName = deviceName;
    s.permanentUnbind.store(false, std::memory_order_release);

    // If the engine is not running, we opened only to VALIDATE eagerly (so an
    // unplugged / wrong-rate device fails the bind NOW instead of silently dropping
    // at the next startAudio). Close it again so a stopped engine never leaves an
    // interface capturing in the background; reopenDesiredExtraInputs() re-opens it
    // (and re-attaches the callback) when the engine next starts.
    if (! audioRunning.load(std::memory_order_relaxed))
    {
        s.manager.closeAudioDevice();
        return {};
    }

    // Attach the callback — fires extraInputAboutToStart (prepares + flips active).
    // Any stale registration was already removed before the open above, so this
    // registers exactly once.
    s.manager.addAudioCallback(&s.callback);
    return {};
}

bool AudioEngine::unbindInputDevice(int deviceKey)
{
    if (deviceKey < 1 || deviceKey > kMaxExtraInputDevices)
        return false;
    const int slot = deviceKey - 1;
    // User-initiated unbind: mark it PERMANENT (the device thread's extraInputStopped
    // reads this atomic to deactivate the slot's sources) BEFORE closing, and forget
    // the INTENT so a later startAudio() does not resurrect a device the user
    // deliberately removed.
    extraInputs[(size_t) slot].permanentUnbind.store(true, std::memory_order_release);
    extraInputs[(size_t) slot].desiredDeviceName = {};
    // If the device is open, closing it fires extraInputStopped(), which — with the
    // intent now cleared — deactivates this deviceKey's sources. If it was ALREADY
    // closed (e.g. a prior stopAudio() kept the intent + left the sources active for
    // a resume that will now never come), extraInputStopped() will NOT run, so we
    // must deactivate them here — otherwise they linger as ghost sources stranding
    // pool slots and showing in listSources().
    if (! closeExtraInputDevice(slot))
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        for (auto& src : sources)
            if (src->isActive() && src->getDeviceKey() == deviceKey)
            {
                src->releaseResources();
                src->setActive(false);
            }
    }
    return true;
}

// Close the device open on a slot WITHOUT forgetting desiredDeviceName, so
// startAudio() re-opens it. Used by stopAudio()/reconfigure (transient close) — the
// public unbindInputDevice() clears the intent first (permanent removal).
bool AudioEngine::closeExtraInputDevice(int slot)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices)
        return false;
    InputDeviceSlot& s = extraInputs[(size_t) slot];
    const bool wasActive = s.active.load(std::memory_order_acquire);
    // Close + deregister UNCONDITIONALLY (not gated on `active`). An UNPLANNED stop
    // (USB unplug / backend restart) fires extraInputStopped() — flipping active
    // false — yet leaves the manager owning a (possibly auto-recovering) device and
    // s.callback still registered. If we no-oped on !active, stopAudio()/reconfigure
    // would never release it and the backend could resume callbacks after the engine
    // is supposedly stopped. Both calls are idempotent when already closed/absent.
    // For an ACTIVE slot, closeAudioDevice() blocks for the callback thread then fires
    // audioDeviceStopped → extraInputStopped (releases this device's sources).
    s.manager.closeAudioDevice();
    s.manager.removeAudioCallback(&s.callback);
    return wasActive;
}

// Re-open every slot that has a desiredDeviceName but is not currently active — the
// post-(re)start restore of extra inputs. No-op in duplex (extras need split) and
// when nothing is desired (the single-device path). Called from startAudio().
void AudioEngine::reopenDesiredExtraInputs()
{
    if (duplexMode.load(std::memory_order_relaxed))
    {
        // Duplex has no consumer for extra-device rings, so the desired extras cannot
        // open right now. PRESERVE their intent (so a later switch back to split
        // auto-restores them — setAudioDevices() promises bindings survive a device
        // change) and keep their sources active to resume in place; but ZERO their
        // meters so getSourceLevels() reports silence while the device is gone (the
        // renderer's per-source silence gate then won't treat a temporarily-unavailable
        // source as still hearing audio, and there is no false detection). The sources
        // are not "ghosts": split-restore reopens the device and they resume.
        for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
        {
            if (extraInputs[(size_t) (dk - 1)].desiredDeviceName.isEmpty())
                continue;
            std::lock_guard<std::mutex> lock(sourcesMutex);
            for (auto& src : sources)
                if (src->isActive() && src->getDeviceKey() == dk)
                    src->resetInputMeters();
        }
        return;
    }
    for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
    {
        InputDeviceSlot& s = extraInputs[(size_t) (dk - 1)];
        if (s.desiredDeviceName.isEmpty() || s.active.load(std::memory_order_acquire))
            continue;
        const juce::String err = bindInputDevice(dk, s.desiredDeviceName);  // re-sets desired (idempotent)
        if (err.isNotEmpty())
        {
            // Reopen failed — the interface was unplugged, or no longer supports the
            // engine rate. The transient close kept this slot's sources ACTIVE to
            // resume; since they now never will, give up cleanly: drop the intent and
            // deactivate them so they do not linger as ghost sources stranding pool
            // slots. The renderer re-binds + re-adds if the device returns.
            s.desiredDeviceName = {};
            std::lock_guard<std::mutex> lock(sourcesMutex);
            for (auto& src : sources)
                if (src->isActive() && src->getDeviceKey() == dk)
                    src->setActive(false);
        }
    }
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

    constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
    constexpr uint64_t kCap  = (uint64_t) kOutputRingFrames;

    // Clamp the working size to the scratch capacity pre-allocated in
    // audioOutputAboutToStart() so the .assign() calls below never realloc
    // on the RT thread when a transient oversized block arrives (mirrors
    // the backing path's clamp in audioDeviceIOCallbackWithContext).
    const int scratchCap = (int) outputPullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = outputRingReadIndex.load(std::memory_order_relaxed);
    const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);

    // If audioDeviceStopped() raced between our two loads and reset both
    // indices to 0, we can observe w < r. Treat that as an empty ring
    // and resync — without this, the unsigned (w - r) wraps into a huge
    // positive value and falls into the catch-up branch reading stale slots.
    if (w < r)
    {
        r = w;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
    }

    // Catch up if the producer has lapped (drop-oldest is achieved via this
    // single-writer consumer-side advance, not a producer-side write to r).
    if ((w - r) > kCap)
    {
        r = w - kCap;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
        inputOverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

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
        const uint64_t slot = (r + (uint64_t) i) & kMask;
        // Single atomic load → atomic unpack of both channels (matches the
        // producer's packed store) so L and R always belong to the same frame.
        float l, rr;
        unpackLR(outputPendingRing[slot].load(std::memory_order_relaxed), l, rr);
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
    outputRingReadIndex.store(r + (uint64_t) consumeCount, std::memory_order_release);

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
    for (auto& s : extraInputs)
    {
        if (! s.active.load(std::memory_order_acquire)) continue;
        uint64_t er = s.readIndex.load(std::memory_order_relaxed);
        const uint64_t ew = s.writeIndex.load(std::memory_order_acquire);
        if (ew < er) { er = ew; s.readIndex.store(er, std::memory_order_relaxed); }
        if ((ew - er) > kCap)
        {
            er = ew - kCap;
            s.readIndex.store(er, std::memory_order_relaxed);
            s.overflowCount.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t eAvail   = ew - er;
        const int      ePull    = juce::jmin(outSamples, (int) eAvail);
        const int      eConsume = juce::jmin(numSamples,  (int) eAvail);
        for (int i = 0; i < ePull; ++i)
        {
            const uint64_t slot = (er + (uint64_t) i) & kMask;
            float l, rr;
            unpackLR(s.ring[(size_t) slot].load(std::memory_order_relaxed), l, rr);
            buffer.addSample(0, i, l);
            if (copyChannels > 1) buffer.addSample(1, i, rr);
        }
        s.readIndex.store(er + (uint64_t) eConsume, std::memory_order_release);
    }

    // Stream sink (producer, split clock): snapshot the full guitar mix (primary
    // ring + extra inputs) BEFORE backing is added.
    const bool streamActive = streamSink.active.load(std::memory_order_acquire);
    int   streamBackingFrames = 0;
    float streamBackingVol    = 0.0f;
    bool  streamBackingOn     = false;
    if (streamActive && streamGuitarScratch.getNumSamples() >= numSamples)
        for (int ch = 0; ch < 2; ++ch)
            streamGuitarScratch.copyFrom(ch, 0, buffer,
                juce::jmin(ch, buffer.getNumChannels() - 1), 0, numSamples);

    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            // Shared with the duplex path so the two callbacks can't drift.
            const int backingOut = renderBackingBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            streamBackingFrames = backingOut; streamBackingVol = bVol; streamBackingOn = true;
            // RMS, computed identically to the duplex path so getBackingLevel()
            // reports the same metric regardless of which device clock is active.
            // copyChannels is already capped at 2, so it doubles as the mix count.
            float backingLevelSq = 0.0f;
            for (int ch = 0; ch < copyChannels; ++ch)
            {
                const float* const src = backingBuffer.getReadPointer(ch);
                float sumSquares = 0.0f;
                for (int i = 0; i < backingOut; ++i)
                    sumSquares += src[i] * src[i];

                const float channelRms = (backingOut > 0)
                    ? std::sqrt(sumSquares / backingOut) * bVol
                    : 0.0f;
                backingLevelSq += channelRms * channelRms;
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, backingOut, bVol);
            }
            currentBackingLevel.store((copyChannels > 0)
                ? std::sqrt(backingLevelSq / copyChannels)
                : 0.0f);
        }
        else
        {
            currentBackingLevel.store(0.0f);
        }

        // Stream sink: compose + push the stream submix before the local master
        // gain. Done INSIDE the backingLock scope so backingBuffer is read under the
        // very lock that guards its resize (audio*AboutToStart) — matching the duplex
        // path. When the tryLock failed (streamBackingOn=false) we pass a null backing
        // pointer and never touch backingBuffer, so there is nothing to protect.
        if (streamActive)
            composeAndPushStreamMix(streamGuitarScratch,
                                    streamBackingOn ? &backingBuffer : nullptr,
                                    streamBackingFrames, streamBackingVol, numSamples);
    }

    buffer.applyGain(outputGain.load());

    float peak = 0.0f;
    for (int ch = 0; ch < numOutputChannels; ++ch)
        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
    currentOutputLevel.store(peak);
    float prevPeak = outputPeak.load();
    if (peak > prevPeak) outputPeak.store(peak);
}
