// DeviceSetup implementation — moved verbatim from AudioEngine.cpp (TLC plan
// phase 4 / §2.7). The only edits beyond member renames are the extraction of
// the three previously hand-synced helpers (ratesMatch / resolveDeviceName /
// rateSupportedBy), which each site now calls instead of open-coding.

#include "DeviceSetup.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace slopsmith {

juce::String DeviceSetup::resolveDeviceName(juce::AudioIODeviceType* t,
                                            bool isInput, const juce::String& name)
{
    if (t == nullptr || name.isNotEmpty()) return name;
    auto names = t->getDeviceNames(isInput);
    return names.size() > 0 ? names[0] : name;
}

bool DeviceSetup::rateSupportedBy(juce::AudioIODeviceType* t, const juce::String& dev,
                                  bool isInput, double sr)
{
    // v1 forces matching nominal SR — no adaptive resampler yet. Resolve empty
    // name to first-enumerated for the createDevice probe call (matches
    // probeDual's strategy). createDevice("") is implementation-defined per
    // backend — some return the default, some return null. Using
    // first-enumerated keeps probe and apply checking the SAME concrete
    // device, so an empty-name config can't pass the UI probe and then fail
    // this check.
    if (!t) return false;
    const juce::String resolved = resolveDeviceName(t, isInput, dev);
    std::unique_ptr<juce::AudioIODevice> probe(
        isInput ? t->createDevice({}, resolved) : t->createDevice(resolved, {}));
    if (!probe) return false;
    // Tolerance matches the probe-side rounding: probeDual rounds the matched
    // rate to the nearest integer, so a backend reporting e.g. 47999.5
    // surfaces 48000 in the UI. If we kept `< 0.5` here, the round-trip would
    // fail at apply time because |47999.5 - 48000.0| is exactly 0.5.
    for (auto r : probe->getAvailableSampleRates())
        if (ratesMatch(r, sr)) return true;
    return false;
}

DeviceOptions DeviceSetup::probeDual(const juce::String& inputTypeName,
                                     const juce::String& inputName,
                                     const juce::String& outputTypeName,
                                     const juce::String& outputName)
{
    DeviceOptions options;
    options.inputType = inputTypeName;
    options.outputType = outputTypeName.isEmpty() ? inputTypeName : outputTypeName;
    options.type = options.inputType;   // legacy alias

    // Resolve each side from its own manager so probe stays consistent with
    // applySplit()/setOutputDeviceType(), which mutate the manager that owns
    // the side they're configuring. Using the input manager for the output
    // lookup would silently fall back to whatever input has scanned, which
    // can miss output-only backends.
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

    auto* inputType  = findType(inMgr, options.inputType);

    // Match setAudioDevices's resolution: when the caller didn't specify
    // an output type, default it to the SAME type the input side resolved
    // to (using the type's name, looked up in the output manager). Without
    // this, an empty `options.outputType` would let findType pick whatever
    // the output manager enumerates first — potentially a different backend
    // than the input manager picked from the empty string, which then
    // disagrees with the apply path's duplex classification.
    juce::String effectiveOutputTypeName = options.outputType;
    if (effectiveOutputTypeName.isEmpty() && inputType != nullptr)
        effectiveOutputTypeName = inputType->getTypeName();
    auto* outputType = findType(outMgr, effectiveOutputTypeName);

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

        // For probing we still need a concrete device to instantiate.
        // Resolve empty names to first-enumerated ONLY for the probe-device
        // creation below — DON'T write back into options.input/options.output;
        // those flow to the UI and the apply path, which treat empty as
        // "OS default" per side.
        const juce::String probeInputName  = resolveDeviceName(inputType,  true,  options.input);
        const juce::String probeOutputName = resolveDeviceName(outputType, false, options.output);

        // Probe the SAME way setAudioDevices() will actually apply, or the
        // startup auto-apply mis-fires: init() fail-closes on this probe's
        // `compatible` verdict, so if the probe measures a combined duplex device
        // but apply then opens split (or vice-versa), the verdict describes a
        // config that won't be the one used — the classic symptom being "no audio
        // until I press Apply". Duplex is only attempted for the SAME physical
        // endpoint (a true single-clock device); two different endpoints of the
        // same backend (USB cable in + separate speakers out) are two clocks and
        // go split. Mirror setAudioDevices()'s sameEndpointIntent exactly.
        bool isDuplex = (options.inputType == options.outputType)
                        && (options.input == options.output);

        if (isDuplex)
        {
            std::unique_ptr<juce::AudioIODevice> dev(
                inputType->createDevice(probeOutputName, probeInputName));
            if (dev)
            {
                options.inputChannels = dev->getInputChannelNames();
                options.outputChannels = dev->getOutputChannelNames();
                for (auto rate : dev->getAvailableSampleRates())
                    options.sampleRates.addIfNotAlreadyThere(rate);
                for (auto size : dev->getAvailableBufferSizes())
                    options.bufferSizes.addIfNotAlreadyThere(size);
            }
            else
            {
                isDuplex = false;
            }
        }
        if (!isDuplex)
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

            // Tolerance covers backends that report fractional drift around
            // the nominal rate — ratesMatch is the same <= 0.5 the apply-side
            // rateSupportedBy check uses, so the probe can't reject a
            // boundary case the apply would accept (or vice versa).
            const auto inRates = inDev->getAvailableSampleRates();
            const auto outRates = outDev->getAvailableSampleRates();
            for (auto r : inRates)
            {
                for (auto r2 : outRates)
                {
                    if (ratesMatch(r, r2))
                    {
                        // Midpoint-rounded clean nominal, fail-closed when the
                        // rounded value falls outside tolerance of either side
                        // — see nominalRateCandidate (RateMatch.h).
                        double candidate = 0.0;
                        if (nominalRateCandidate(r, r2, candidate))
                            options.sampleRates.addIfNotAlreadyThere(candidate);
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

juce::String DeviceSetup::applyDuplex(const juce::String& inputName,
                                      const juce::String& outputName,
                                      double sampleRate, int bufferSize,
                                      SourceChain& monitorChain)
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
    if (auto* currentDevice = inMgr.getCurrentAudioDevice())
    {
        try
        {
            juce::AudioDeviceManager::AudioDeviceSetup current;
            inMgr.getAudioDeviceSetup(current);

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
                && state.duplexMode.load(std::memory_order_relaxed))
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
    if (auto* currentType = inMgr.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();
    if (inMgr.getCurrentAudioDevice() != nullptr)
    {
        try {
            inMgr.closeAudioDevice();
            fprintf(stderr, "[AudioEngine] Closed device for reconfiguration\n");
            if (currentTypeName.isNotEmpty())
                inMgr.setCurrentAudioDeviceType(currentTypeName, true);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] closeAudioDevice crashed, continuing\n");
        }
    }
#endif

    int inputChannelCount = 0;
    int outputChannelCount = 0;
    if (auto* type = inMgr.getCurrentDeviceTypeObject())
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
        result = inMgr.setAudioDeviceSetup(setup, true);
    } catch (...) {
        return "setAudioDeviceSetup threw";
    }
    if (result.isNotEmpty())
    {
        fprintf(stderr, "[AudioEngine] Device setup error: %s\n", result.toRawUTF8());
        try {
            result = inMgr.initialiseWithDefaultDevices(2, 2);
        } catch (...) {
            return "fallback initialiseWithDefaultDevices threw";
        }
        if (result.isNotEmpty())
            return "device setup failed: " + result;
    }

    if (auto* configuredDevice = inMgr.getCurrentAudioDevice())
    {
        const double sr = configuredDevice->getCurrentSampleRate();
        const int bs = configuredDevice->getCurrentBufferSizeSamples();
        state.currentSampleRate.store(sr, std::memory_order_relaxed);
        state.inputBlockSize.store(bs, std::memory_order_relaxed);
        state.outputBlockSize.store(bs, std::memory_order_relaxed);

        fprintf(stderr, "[AudioEngine] Duplex device configured OK. Current device: %s\n",
                configuredDevice->getName().toRawUTF8());
        fprintf(stderr, "[AudioEngine] Actual device setup: sr=%.0f bs=%d (requested bs=%d)\n",
                sr, bs, bufferSize);

        monitorChain.prepareMonitorChain(sr, bs);
        return {};
    }
    state.currentSampleRate.store(0.0, std::memory_order_relaxed);
    state.inputBlockSize.store(0, std::memory_order_relaxed);
    state.outputBlockSize.store(0, std::memory_order_relaxed);
    monitorChain.releaseMonitorChain();
    return "no current device after setup";
}

DeviceConfigResult DeviceSetup::applySplit(const DeviceConfig& config,
                                           SourceChain& monitorChain,
                                           OutputRing& outputRing,
                                           std::atomic<uint64_t>& outputUnderflowCount,
                                           std::atomic<uint64_t>& inputOverflowCount,
                                           juce::AudioIODeviceCallback& outputCallback,
                                           bool& outputCallbackRegistered)
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
        if (auto* current = outMgr.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != config.outputType)
                outMgr.setCurrentAudioDeviceType(config.outputType, true);
        }
        else
        {
            outMgr.setCurrentAudioDeviceType(config.outputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for output type '" + config.outputType + "'";
        return res;
    }

    juce::AudioIODeviceType* inputType = nullptr;
    juce::AudioIODeviceType* outputType = nullptr;
    for (auto* t : inMgr.getAvailableDeviceTypes())
        if (t->getTypeName() == config.inputType) { inputType = t; break; }
    for (auto* t : outMgr.getAvailableDeviceTypes())
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
    // rateSupportedBy preflight above AND probeDual. Using empty +
    // useDefault*Channels here would make JUCE open the OS default, which can
    // differ from inputs[0] on platforms where the OS-default differs from
    // JUCE's enumeration order. The probe + SR preflight + actual open all
    // need to agree on the same concrete device for the apply path to behave
    // consistently with what the UI showed the user.
    const juce::String resolvedInputName = resolveDeviceName(inputType, true, config.inputDevice);

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
            try { outMgr.removeAudioCallback(&outputCallback); } catch (...) {}
            outputCallbackRegistered = false;
        }
        try { inMgr.closeAudioDevice(); } catch (...) {}
        try { outMgr.closeAudioDevice(); } catch (...) {}
    };

    // Mirror applyDuplex's JUCE_LINUX close-before-reconfigure pattern:
    // ALSA deadlocks if we let setAudioDeviceSetup mutate a live device. The
    // device type is re-asserted afterwards so the close doesn't drop us back
    // to whatever JUCE picked at startup. closeAudioDevice/setCurrentAudioDeviceType
    // throwing is non-fatal — we still try the setup below and surface its error.
#if JUCE_LINUX
    {
        juce::String currentInputTypeName;
        if (auto* currentType = inMgr.getCurrentDeviceTypeObject())
            currentInputTypeName = currentType->getTypeName();
        if (inMgr.getCurrentAudioDevice() != nullptr)
        {
            try {
                inMgr.closeAudioDevice();
                if (currentInputTypeName.isNotEmpty())
                    inMgr.setCurrentAudioDeviceType(currentInputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode input close threw, continuing\n");
            }
        }
    }
#endif

    juce::String inErr;
    try { inErr = inMgr.setAudioDeviceSetup(inSetup, true); }
    catch (...) { res.error = "input setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (inErr.isNotEmpty()) { res.error = "input setup: " + inErr; rollbackOpenedDevices(); return res; }

    auto* inDev = inMgr.getCurrentAudioDevice();
    if (!inDev) { res.error = "no input device after setup"; rollbackOpenedDevices(); return res; }
    const double inSr = inDev->getCurrentSampleRate();
    const int    inBs = inDev->getCurrentBufferSizeSamples();

    // Same first-enumerated resolution on the output side — see input note
    // above for why this matches the probe + SR preflight strategy.
    const juce::String resolvedOutputName = resolveDeviceName(outputType, false, config.outputDevice);

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
        if (auto* currentType = outMgr.getCurrentDeviceTypeObject())
            currentOutputTypeName = currentType->getTypeName();
        if (outMgr.getCurrentAudioDevice() != nullptr)
        {
            try {
                outMgr.closeAudioDevice();
                if (currentOutputTypeName.isNotEmpty())
                    outMgr.setCurrentAudioDeviceType(currentOutputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode output close threw, continuing\n");
            }
        }
    }
#endif

    juce::String outErr;
    try { outErr = outMgr.setAudioDeviceSetup(outSetup, true); }
    catch (...) { res.error = "output setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (outErr.isNotEmpty()) { res.error = "output setup: " + outErr; rollbackOpenedDevices(); return res; }

    auto* outDev = outMgr.getCurrentAudioDevice();
    if (!outDev) { res.error = "no output device after setup"; rollbackOpenedDevices(); return res; }
    const double outSr = outDev->getCurrentSampleRate();
    const int    outBs = outDev->getCurrentBufferSizeSamples();

    if (!ratesMatch(inSr, outSr))
    {
        res.error = "Input and output devices opened at different sample rates";
        rollbackOpenedDevices();
        return res;
    }

    state.currentSampleRate.store(inSr, std::memory_order_relaxed);
    state.inputBlockSize.store(inBs, std::memory_order_relaxed);
    state.outputBlockSize.store(outBs, std::memory_order_relaxed);

    fprintf(stderr, "[AudioEngine] Split mode configured: inSr=%.0f inBs=%d outSr=%.0f outBs=%d\n",
            inSr, inBs, outSr, outBs);

    outputRing.reset();
    outputUnderflowCount.store(0, std::memory_order_relaxed);
    inputOverflowCount.store(0, std::memory_order_relaxed);

    monitorChain.prepareMonitorChain(inSr, inBs);

    res.ok = true;
    res.sampleRate = inSr;
    res.inputBlockSize = inBs;
    res.outputBlockSize = outBs;
    return res;
}

void DeviceSetup::teardownSplit(OutputRing& outputRing,
                                juce::AudioIODeviceCallback& outputCallback,
                                bool& outputCallbackRegistered)
{
    // Unconditional remove — JUCE's removeAudioCallback is idempotent
    // (no-op if the callback isn't registered), so we don't need the
    // outputCallbackRegistered guard here. This makes teardown robust
    // against a stale flag left over from a previous failed split setup.
    outMgr.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    try { outMgr.closeAudioDevice(); }
    catch (...) { fprintf(stderr, "[AudioEngine] teardownSplitMode: output close threw\n"); }

    outputRing.reset();
}

} // namespace slopsmith
