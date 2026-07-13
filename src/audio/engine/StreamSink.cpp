// StreamSink implementation — moved verbatim from AudioEngine.cpp (TLC plan
// phase 2 / §2.5); member names lose their streamSink./streamBus prefixes,
// logic is unchanged. See StreamSink.h for the design rationale.

#include "StreamSink.h"

#include <cmath>
#include <cstdio>

namespace slopsmith {

void StreamSink::publish(const juce::AudioBuffer<float>& guitarMix,
                         const juce::AudioBuffer<float>* backingBuf, int backingFrames, float backingVol,
                         const juce::AudioBuffer<float>* rendererBuf, int rendererFrames,
                         int numSamples)
{
    if (! active.load(std::memory_order_acquire)) return;
    // A block larger than the entire ring can't be published atomically (it would
    // wrap and overwrite unread slots before writeIndex is bumped). Skip it and
    // count an overflow. Checked FIRST (before the scratch guard) so an oversized
    // block is always counted — the fixed-size scratch is exactly the ring, so an
    // oversized block also trips the scratch guard below and would otherwise be
    // dropped silently. The split path already rejects oversized devices at setup;
    // this guards the duplex path, whose block size we don't pre-validate.
    if (numSamples > kRingFrames)
    {
        overflowCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Scratch not yet sized to the full ring (cold start before the producer's
    // about-to-start ran, or a transient reconfig) — skip rather than alloc on RT.
    // After warm-up the scratch is exactly the ring, so for an in-range block this
    // never trips.
    if (mixScratch.getNumSamples() < numSamples) return;

    const bool  ig   = busIncludeGuitar.load(std::memory_order_relaxed);
    const bool  ib   = busIncludeBacking.load(std::memory_order_relaxed);
    const float gain = busGain.load(std::memory_order_relaxed);

    mixScratch.clear(0, 0, numSamples);
    mixScratch.clear(1, 0, numSamples);
    if (ig)
        for (int ch = 0; ch < 2; ++ch)
            mixScratch.addFrom(ch, 0, guitarMix,
                juce::jmin(ch, guitarMix.getNumChannels() - 1), 0, numSamples);
    if (ib && backingBuf != nullptr && backingFrames > 0)
    {
        const int n = juce::jmin(backingFrames, numSamples);
        for (int ch = 0; ch < 2; ++ch)
            mixScratch.addFrom(ch, 0, *backingBuf,
                juce::jmin(ch, backingBuf->getNumChannels() - 1), 0, n, backingVol);
    }
    // Renderer-fed song audio (stems / element / loopback riding the renderer
    // bus) is song audio for the streamer too — without this the stream mix
    // carries guitar only whenever the song bypasses the native transport
    // (multi-stem under exclusive/ASIO output). Bus gain is already applied by
    // pullRendererBus; only the stream gain below shapes it further. Backing
    // transport and renderer bus are mutually exclusive song paths in
    // practice, so this never double-carries.
    if (ib && rendererBuf != nullptr && rendererFrames > 0)
    {
        const int n = juce::jmin(rendererFrames, numSamples);
        for (int ch = 0; ch < 2; ++ch)
            mixScratch.addFrom(ch, 0, *rendererBuf,
                juce::jmin(ch, rendererBuf->getNumChannels() - 1), 0, n);
    }
    mixScratch.applyGain(0, 0, numSamples, gain);
    mixScratch.applyGain(1, 0, numSamples, gain);

    const float peak = juce::jmax(mixScratch.getMagnitude(0, 0, numSamples),
                                  mixScratch.getMagnitude(1, 0, numSamples));
    level.store(peak, std::memory_order_relaxed);

    ring.push(mixScratch.getReadPointer(0), mixScratch.getReadPointer(1), numSamples);
}

void StreamSink::deviceCallback(float* const* outputData, int numOutputChannels, int numSamples)
{
    const juce::ScopedNoDenormals noDenormals;
    if (numOutputChannels <= 0) return;
    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);
    buffer.clear();

    const int scratchCap = (int) pullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = ring.readIndex.load(std::memory_order_relaxed);
    const uint64_t w = ring.writeIndex.load(std::memory_order_acquire);
    ring.resyncIfIndicesReset(r, w);
    if (ring.catchUpIfLapped(r, w))
        overflowCount.fetch_add(1, std::memory_order_relaxed);
    const uint64_t available   = w - r;
    const int      pullCount   = juce::jmin(outSamples, (int) available);
    const int      consumeCount = juce::jmin(numSamples, (int) available);
    const int      copyChannels = juce::jmin(numOutputChannels, 2);
    for (int i = 0; i < pullCount; ++i)
    {
        float l, rr;
        ring.readFrame(r + (uint64_t) i, l, rr);
        buffer.setSample(0, i, l);
        if (copyChannels > 1) buffer.setSample(1, i, rr);
    }
    if (pullCount < outSamples)
        underflowCount.fetch_add(1, std::memory_order_relaxed);
    ring.commitRead(r + (uint64_t) consumeCount);
}

void StreamSink::deviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr) return;
    const int bs = device->getCurrentBufferSizeSamples();
    double sr = device->getCurrentSampleRate();
    if (sr <= 0.0) sr = state.currentSampleRate.load(std::memory_order_relaxed);
    sinkBlockSize.store(bs, std::memory_order_relaxed);
    sinkSampleRate.store(sr, std::memory_order_relaxed);
    const int cap = juce::jmax(bs, 2048);
    if ((int) pullScratchL.size() < cap) pullScratchL.assign((size_t) cap, 0.0f);
    if ((int) pullScratchR.size() < cap) pullScratchR.assign((size_t) cap, 0.0f);
    ring.reset();
    underflowCount.store(0, std::memory_order_relaxed);
    overflowCount.store(0, std::memory_order_relaxed);
}

void StreamSink::deviceStopped()
{
    // Fires on any stop of the stream device — an unplanned loss (unplug / driver
    // reset) as well as our own teardown. Mark inactive so the producer stops
    // filling a now-consumer-less ring and the meter clears; desiredTypeName/Name
    // are deliberately left intact so reopenDesired() can restore it.
    active.store(false, std::memory_order_release);
    level.store(0.0f, std::memory_order_relaxed);
}

juce::String StreamSink::open(const juce::String& typeName, const juce::String& deviceName)
{
    // Control-thread only. Opens an OUTPUT-only device on the stream sink's own
    // AudioDeviceManager and attaches the drain callback. Mirrors applySplitSetup's
    // output open. v1 requires the sink's nominal SR to match the engine rate (no
    // async resampler yet — that's PR3); a mismatch is rejected with a clear error.
    desiredTypeName = typeName;
    desiredDeviceName = deviceName;

    // Stop the producer from writing the ring while we (re)configure the device:
    // setAudioDeviceSetup() below drives deviceAboutToStart(), which resets the
    // ring indices and slots. Clearing `active` first stops the main callback from
    // STARTING new pushes. A producer block already past the active-check can still
    // finish one push, but the device close/reopen takes far longer than a single
    // audio block, so that in-flight push completes well before about-to-start runs.
    // Worst case is therefore one imperfect block on the STREAM bus (never the local
    // monitor) during a manual device switch — atomic, no data race, no UAF. We only
    // re-arm `active` after a fully clean open.
    active.store(false, std::memory_order_release);

    // Any failure below: detach/close the half-open device AND drop the desired
    // intent. A deterministic failure (e.g. SR mismatch) is then NOT retried on every
    // startAudio(), and the engine never reports active with no device behind it. The
    // renderer keeps its own persisted choice and re-applies it, so nothing is lost.
    auto fail = [this](const juce::String& msg) -> juce::String {
        close();
        desiredTypeName = {};
        desiredDeviceName = {};
        return msg;
    };

    if (! initialised)
    {
        manager.initialise(0, 2, nullptr, false);
        initialised = true;
    }

    juce::AudioIODeviceType* outType = nullptr;
    for (auto* t : manager.getAvailableDeviceTypes())
        if (t->getTypeName() == typeName) { outType = t; break; }
    if (! outType) return fail("Stream output device type not found: " + typeName);

    try {
        if (auto* cur = manager.getCurrentDeviceTypeObject())
        {
            if (cur->getTypeName() != typeName)
                manager.setCurrentAudioDeviceType(typeName, true);
        }
        else manager.setCurrentAudioDeviceType(typeName, true);
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
    setup.sampleRate = state.currentSampleRate.load(std::memory_order_relaxed);
    setup.bufferSize = state.outputBlockSize.load(std::memory_order_relaxed);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.setRange(0, 2, true);

    juce::String err;
    try { err = manager.setAudioDeviceSetup(setup, true); }
    catch (...) { return fail("stream output setAudioDeviceSetup threw"); }
    if (err.isNotEmpty()) return fail("stream output setup: " + err);

    auto* dev = manager.getCurrentAudioDevice();
    if (! dev) return fail("no stream output device after setup");
    const double devSr = dev->getCurrentSampleRate();
    const double engineSr = state.currentSampleRate.load(std::memory_order_relaxed);
    if (engineSr > 0.0 && std::abs(devSr - engineSr) > 0.5)
        return fail("Stream output sample rate (" + juce::String(devSr)
             + ") must match the engine rate (" + juce::String(engineSr)
             + "). Pick a device that supports " + juce::String(engineSr) + " Hz.");

    if (! callbackRegistered)
    {
        manager.addAudioCallback(&callback);
        callbackRegistered = true;
    }
    active.store(true, std::memory_order_release);
    fprintf(stderr, "[AudioEngine] stream output active: %s (%s)\n",
            resolved.toRawUTF8(), typeName.toRawUTF8());
    return {};
}

void StreamSink::close()
{
    // Detach the drain callback and close the device, leaving desiredTypeName/Name
    // intact so startAudio()/reopenDesired() can restore it. active=false
    // first so the producer stops pushing; removeAudioCallback() then blocks until
    // the consumer callback is no longer in flight, so the ring/device go quiescent
    // before close. Idempotent — safe when nothing is open.
    active.store(false, std::memory_order_release);
    if (callbackRegistered)
    {
        manager.removeAudioCallback(&callback);
        callbackRegistered = false;
    }
    try { manager.closeAudioDevice(); } catch (...) {}
    level.store(0.0f, std::memory_order_relaxed);
}

void StreamSink::clear()
{
    close();
    desiredTypeName = {};
    desiredDeviceName = {};
}

void StreamSink::reopenDesired()
{
    // Copy the intent first: open() mutates desiredTypeName/Name (and clears
    // them on failure), so don't pass the members in by reference.
    const juce::String t = desiredTypeName;
    const juce::String d = desiredDeviceName;
    if (d.isEmpty() && t.isEmpty()) return;
    const juce::String err = open(t, d);
    if (err.isNotEmpty())
        fprintf(stderr, "[AudioEngine] reopenDesiredStreamSink failed: %s\n", err.toRawUTF8());
}

} // namespace slopsmith
