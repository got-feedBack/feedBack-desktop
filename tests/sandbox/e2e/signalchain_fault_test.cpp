// Integration test for the in-process plugin fault guard in SignalChain.cpp.
//
// Drives a deliberately-segfaulting in-process AudioProcessor through a REAL
// SignalChain::process() and asserts that:
//   1. the host process SURVIVES the plugin's SIGSEGV (POSIX guard / SEH),
//   2. the faulting processor is released from its slot,
//   3. the plugin is added to the runtime crash blocklist, so shouldSandbox()
//      now routes it out-of-process on its next load (self-healing), and
//   4. a subsequent process() call is safe (the dead slot is skipped).
//
// This is the end-to-end counterpart to the standalone mechanism check: it
// exercises the actual invokePlugin() guard, not a copy. POSIX-only harness
// (matches the sandbox e2e job); on Windows the same path is covered by the
// /EHa SEH catch.

#include "SignalChain.h"
#include "Sandbox/SandboxedProcessor.h"   // slopsmith::sandbox::{shouldSandbox,addCrashedPlugin}

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <stdexcept>

namespace {

// Base fixture: a minimal in-process AudioProcessor. Subclasses decide how
// processBlock() fails.
class FaultFixture : public juce::AudioProcessor
{
public:
    FaultFixture()
        : juce::AudioProcessor(BusesProperties()
              .withInput("In",   juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "FaultGuardFixture"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

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

// Faults with a hardware signal (SIGSEGV) — the POSIX guard / Windows SEH path.
class SignalFaultingProcessor : public FaultFixture
{
public:
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        // Deliberate null dereference → SIGSEGV. volatile so the compiler can't
        // elide it; the asm barrier keeps it from being hoisted/removed under -O2.
        volatile int* p = nullptr;
        *p = 0xC0FFEE;
        asm volatile("" ::: "memory");
    }
};

// Faults with a normal C++ exception — the path that left the POSIX guard armed
// with a stale landing pad before the fix (catch must restore the guard).
class ThrowingProcessor : public FaultFixture
{
public:
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        throw std::runtime_error("deliberate plugin exception");
    }
};

bool fail(const char* msg) { std::printf("FAIL: %s\n", msg); return false; }

// Drive one faulting processor through a fresh chain; assert the host survives,
// the processor is released, and the plugin is blocklisted.
template <typename ProcT>
bool runFaultScenario(const char* label, const char* fileName)
{
    // A unique on-disk path ending in .vst3 so shouldSandbox() treats it as a
    // VST3 (non-VST3 paths always stay in-process and skip the blocklist).
    const juce::String fakePath = juce::File::getCurrentWorkingDirectory()
                                      .getChildFile(fileName).getFullPathName();
    juce::PluginDescription desc;
    desc.fileOrIdentifier = fakePath;

    if (slopsmith::sandbox::shouldSandbox(desc))
        return fail("fixture was already blocklisted before the fault");

    SignalChain chain;
    chain.prepare(48000.0, 256);
    const int slotId = chain.addProcessor(std::make_unique<ProcT>(),
                                          ProcessorSlot::Type::VST, label, fakePath);
    if (chain.getNumSlots() != 1)
        return fail("processor was not added to the chain");

    juce::AudioBuffer<float> buffer(2, 256);
    buffer.clear();
    juce::MidiBuffer midi;

    // The crux: this drives the faulting plugin and MUST return rather than die.
    chain.process(buffer, midi);
    std::printf("  [%s] survived\n", label);

    const ProcessorSlot* slot = chain.getSlot(slotId);
    if (slot == nullptr)             return fail("slot vanished after the fault");
    if (slot->processor != nullptr)  return fail("faulting processor was not released");
    if (! slopsmith::sandbox::shouldSandbox(desc))
        return fail("plugin was not blocklisted after faulting");

    // A second block must be safe: the dead slot is simply skipped.
    chain.process(buffer, midi);
    return true;
}

bool runTest()
{
    // Signal fault (SIGSEGV) and C++-exception fault, each through its own chain.
    // Running both in one process also exercises that the C++-exception path
    // leaves the per-thread POSIX guard in a clean (disarmed) state — a stale
    // armed flag from the first scenario would corrupt the second.
    if (! runFaultScenario<SignalFaultingProcessor>("signal-fault", "FaultGuardSignal.vst3"))
        return false;
    if (! runFaultScenario<ThrowingProcessor>("cxx-exception", "FaultGuardThrow.vst3"))
        return false;

    std::printf("OK: SignalChain survived both fault kinds; processors released + blocklisted\n");
    return true;
}

} // namespace

int main()
{
    const bool ok = runTest();
    return ok ? 0 : 1;
}
