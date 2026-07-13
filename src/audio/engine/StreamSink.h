#pragma once

// StreamSink — the streamer-mix output sink (TLC plan phase 2 / §2.5, was
// "PR1" inside AudioEngine). A second OUTPUT AudioDeviceManager on its OWN
// clock that drains a dedicated SPSC ring fed by the main output path's
// composed stream submix (publish()). Mirrors the InputDeviceSlot pattern
// INVERTED to the output side: the PRODUCER is the primary/output callback,
// the CONSUMER is this extra output device's callback. Default off → no
// behaviour change.
//
// Moved verbatim from AudioEngine; the engine keeps thin facades
// (setStreamOutputDevice / clearStreamOutput / setStreamBus / metrics
// getters) so the NodeAddon surface is unchanged. Engine sample rate / output
// block size are read through the EngineState& bound at construction.

#include "PackedStereoRing.h"
#include "EngineState.h"
#include "../GainSanitize.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace slopsmith {

class StreamSink
{
public:
    // Must match the main engine ring capacity: publish() rejects blocks
    // larger than one ring (they can't be published atomically).
    static constexpr int kRingFrames = 4096;

    explicit StreamSink(EngineState& engineState) : state(engineState)
    {
        callback.sink = this;
    }

    // ── Control thread ────────────────────────────────────────────────────
    // Open an OUTPUT-only device and attach the drain callback. Empty error
    // string = success. v1 requires the sink's nominal SR to match the engine
    // rate (no async resampler yet); a mismatch is rejected with a clear error.
    juce::String open(const juce::String& typeName, const juce::String& deviceName);
    // Detach + close but KEEP desiredTypeName/Name so reopenDesired() can
    // restore it after a stop/restart (intent survives). Idempotent.
    void close();
    // close() + drop the desired intent (a user "no stream output").
    void clear();
    void reopenDesired();

    // Size the producer-side scratches to the FIXED ring capacity — call from
    // the engine's about-to-start hooks (device-management thread), never RT.
    // Fixed cap so a hotplug about-to-start can never realloc under a live
    // producer on the other clock; allocates exactly once.
    void prepareProducerScratch()
    {
        if (mixScratch.getNumSamples() < kRingFrames)
            mixScratch.setSize(2, kRingFrames, false, false, true);
    }

    // Bus content: include the backing/game, include the guitar monitor mix,
    // and a linear output gain. All atomic — safe to set live. Gain sanitised
    // (finite, 0..8) so a NaN/Inf from JS can never reach the stream ring.
    void setBus(bool includeBacking, bool includeGuitar, float gain)
    {
        busIncludeBacking.store(includeBacking, std::memory_order_relaxed);
        busIncludeGuitar.store(includeGuitar, std::memory_order_relaxed);
        busGain.store(sanitizeStreamGain(gain), std::memory_order_relaxed);
    }
    void setBusGain(float gain) { busGain.store(sanitizeStreamGain(gain), std::memory_order_relaxed); }

    bool isActive() const { return active.load(std::memory_order_acquire); }
    juce::String getDesiredDeviceName() const { return desiredDeviceName; }
    float getLevel() const { return level.load(std::memory_order_relaxed); }
    uint64_t getUnderflowCount() const { return underflowCount.load(std::memory_order_relaxed); }
    uint64_t getOverflowCount() const { return overflowCount.load(std::memory_order_relaxed); }

    // ── Producer (primary/output callback, RT) ────────────────────────────
    // Compose the stream submix (guitar/backing/renderer × include flags ×
    // gain) into the fixed scratch and pack it into the ring.
    void publish(const juce::AudioBuffer<float>& guitarMix,
                 const juce::AudioBuffer<float>* backingBuf, int backingFrames, float backingVol,
                 const juce::AudioBuffer<float>* rendererBuf, int rendererFrames,
                 int numSamples);

private:
    // Forwards the sink device's callbacks (the sink's own clock).
    struct Callback : juce::AudioIODeviceCallback
    {
        StreamSink* sink = nullptr;
        void audioDeviceIOCallbackWithContext(const float* const* inputData, int numInputChannels,
                                              float* const* outputData, int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ignoreUnused(inputData, numInputChannels);
            if (sink) sink->deviceCallback(outputData, numOutputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* d) override { if (sink) sink->deviceAboutToStart(d); }
        void audioDeviceStopped() override { if (sink) sink->deviceStopped(); }
    };

    // Consumer side (the sink device's thread).
    void deviceCallback(float* const* outputData, int numOutputChannels, int numSamples);
    void deviceAboutToStart(juce::AudioIODevice* device);
    void deviceStopped();

    EngineState& state;

    Callback callback;
    PackedStereoRing<kRingFrames> ring;
    std::atomic<uint64_t> underflowCount{0};
    std::atomic<uint64_t> overflowCount{0};
    std::atomic<bool> active{false};
    std::atomic<double> sinkSampleRate{48000.0};
    std::atomic<int> sinkBlockSize{256};
    std::vector<float> pullScratchL, pullScratchR;  // sized in deviceAboutToStart
    bool callbackRegistered = false;
    bool initialised = false;

    std::atomic<bool>  busIncludeBacking{true};
    std::atomic<bool>  busIncludeGuitar{true};
    std::atomic<float> busGain{1.0f};
    std::atomic<float> level{0.0f};
    // Producer-side composed submix scratch — fixed ring capacity, see
    // prepareProducerScratch().
    juce::AudioBuffer<float> mixScratch;

    // Declared LAST so it DESTRUCTS FIRST (members tear down in reverse
    // declaration order): the manager's dtor closes the device and detaches
    // `callback` while `callback`/`ring` are still alive — no use-after-free
    // even if an explicit teardown path is ever missed. close() also tears it
    // down explicitly before this.
    juce::AudioDeviceManager manager;
    // Persistent INTENT (control thread only): the device the user chose.
    // Survives a stop/restart so reopenDesired() can re-open it.
    juce::String desiredTypeName;
    juce::String desiredDeviceName;
};

} // namespace slopsmith
