// Unit test for the per-slot postGain feature in SignalChain (PR #58).
//
// Drives a REAL SignalChain::process() with an identity in-process processor
// and asserts:
//   1. postGain scales the slot output on every channel (all-channel applyGain),
//   2. a non-finite gain (NaN) is REJECTED by setPostGain (it must never reach
//      the audio buffer — an unclamped NaN would poison the whole chain),
//   3. savePreset() serializes postGain (so a save/load round-trip preserves it;
//      the omission was the bug this PR's review caught), and only when it is
//      non-default (matching the pan/branch "emit non-default only" convention).
//
// No subprocess / VST3 fixture — the processor is a trivial in-process
// AudioProcessor, mirroring signalchain_fault_test.cpp.

#include "SignalChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

// Identity: leaves the buffer untouched, so the test isolates SignalChain's
// post-gain application from any plugin DSP.
class IdentityProcessor : public juce::AudioProcessor
{
public:
    IdentityProcessor()
        : juce::AudioProcessor(BusesProperties()
              .withInput("In",   juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Identity"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

int g_failures = 0;

void check(bool cond, const char* msg)
{
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool approx(float a, float b) { return std::fabs(a - b) < 1.0e-5f; }

int addIdentity(SignalChain& chain)
{
    return chain.addProcessor(std::make_unique<IdentityProcessor>(),
                              ProcessorSlot::Type::VST, "id", "/tmp/identity.vst3");
}

juce::AudioBuffer<float> unityStereo(int numSamples)
{
    juce::AudioBuffer<float> buf(2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buf.setSample(ch, i, 1.0f);
    return buf;
}

} // namespace

int main()
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 128;

    // 1. postGain scales the slot output on both channels.
    {
        SignalChain chain;
        chain.prepare(kSampleRate, kBlockSize);
        const int id = addIdentity(chain);
        check(id >= 0, "addProcessor returns a valid slot id");
        chain.setPostGain(id, 0.5f);

        auto buf = unityStereo(kBlockSize);
        juce::MidiBuffer midi;
        chain.process(buf, midi);
        check(approx(buf.getSample(0, 0), 0.5f) && approx(buf.getSample(1, 0), 0.5f),
              "postGain 0.5 halves both channels (all-channel applyGain)");
    }

    // 2. A NaN gain is rejected — the prior gain stays, and audio stays finite.
    {
        SignalChain chain;
        chain.prepare(kSampleRate, kBlockSize);
        const int id = addIdentity(chain);
        chain.setPostGain(id, 2.0f);
        chain.setPostGain(id, std::nanf(""));   // must be ignored, not stored

        auto buf = unityStereo(kBlockSize);
        juce::MidiBuffer midi;
        chain.process(buf, midi);
        check(std::isfinite(buf.getSample(0, 0)) && approx(buf.getSample(0, 0), 2.0f),
              "NaN gain rejected: prior 2.0 retained, output finite");
    }

    // 3. savePreset serializes postGain only when non-default.
    {
        SignalChain chain;
        chain.prepare(kSampleRate, kBlockSize);
        const int id = addIdentity(chain);

        check(! chain.savePreset().contains("postGain"),
              "default postGain (1.0) is NOT emitted (byte-stable presets)");

        chain.setPostGain(id, 0.25f);
        const juce::String preset = chain.savePreset();
        check(preset.contains("postGain"),
              "non-default postGain is serialized by savePreset (round-trip fix)");
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "TESTS FAILED" : "all tests passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
