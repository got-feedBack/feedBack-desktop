#pragma once

// DeviceSetup — probe/apply/teardown for duplex + split device configs (TLC
// plan phase 4 / §2.7). Moved verbatim from AudioEngine; owns no lifetime —
// it holds references to the engine's two AudioDeviceManagers and its
// EngineState, and the engine-owned collaborators a specific operation needs
// (monitor chain, split output ring, output callback registration) are passed
// by reference at the call. setAudioDevices stays on the AudioEngine facade
// as the orchestrator (stop → resolve → duplex-or-split → restart).
//
// The rate-tolerance (`<= 0.5`, probe/preflight/verify), midpoint-rounding,
// and empty-name→first-enumerated resolution logic that used to live in three
// hand-synced copies is extracted into the shared helpers at the bottom —
// the deep-read §7 dedupe, landed structurally by this move.

#include "EngineState.h"
#include "PackedStereoRing.h"
#include "RateMatch.h"
#include "../SourceChain.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace slopsmith {

// Public device-config shapes — aliased back as AudioEngine::DeviceOptions
// etc., so the NodeAddon surface is unchanged.
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

class DeviceSetup
{
public:
    // Must equal the engine's split-mode ring capacity.
    static constexpr int kOutputRingFrames = 4096;
    using OutputRing = PackedStereoRing<kOutputRingFrames>;

    DeviceSetup(juce::AudioDeviceManager& inputManager,
                juce::AudioDeviceManager& outputManager,
                EngineState& engineState)
        : inMgr(inputManager), outMgr(outputManager), state(engineState) {}

    // Probe what a (typeName, deviceName) pair supports — duplex when input
    // and output are the same endpoint, else the dual/split intersection.
    DeviceOptions probeDual(const juce::String& inputTypeName,
                            const juce::String& inputName,
                            const juce::String& outputTypeName,
                            const juce::String& outputName);

    // Open the combined (single-clock) duplex device on the input manager.
    // Empty error string = success; on success stores the achieved format
    // into EngineState and prepares `monitorChain`.
    juce::String applyDuplex(const juce::String& inputName,
                             const juce::String& outputName,
                             double sampleRate, int bufferSize,
                             SourceChain& monitorChain);

    // Open input-only + output-only devices at a shared nominal rate. On
    // success stores the achieved format, resets the split ring + counters,
    // and prepares `monitorChain`. `outputCallback`/`outputCallbackRegistered`
    // are needed by the partial-open rollback (a failure after the callback
    // was attached must detach it, or the next startAudio() skips re-attach).
    DeviceConfigResult applySplit(const DeviceConfig& config,
                                  SourceChain& monitorChain,
                                  OutputRing& outputRing,
                                  std::atomic<uint64_t>& outputUnderflowCount,
                                  std::atomic<uint64_t>& inputOverflowCount,
                                  juce::AudioIODeviceCallback& outputCallback,
                                  bool& outputCallbackRegistered);

    // Detach the output callback + close the output device + drain the ring.
    void teardownSplit(OutputRing& outputRing,
                       juce::AudioIODeviceCallback& outputCallback,
                       bool& outputCallbackRegistered);

    // ── Shared helpers (the three previously hand-synced copies) ──────────
    // ratesMatch / nominalRateCandidate live in RateMatch.h (JUCE-free, unit-
    // tested); the device-name resolution helpers below need JUCE types.
    // Empty device name → first-enumerated for that type/direction (probe,
    // SR preflight, and split open must all check the SAME concrete device).
    static juce::String resolveDeviceName(juce::AudioIODeviceType* t,
                                          bool isInput, const juce::String& name);
    // Whether `dev` (resolved) supports `sr` within tolerance.
    static bool rateSupportedBy(juce::AudioIODeviceType* t, const juce::String& dev,
                                bool isInput, double sr);

private:
    juce::AudioDeviceManager& inMgr;
    juce::AudioDeviceManager& outMgr;
    EngineState& state;
};

} // namespace slopsmith
