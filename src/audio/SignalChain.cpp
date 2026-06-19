#include "SignalChain.h"
#include "Sandbox/SandboxedProcessor.h"

#if ! JUCE_WINDOWS
 #include <csetjmp>
 #include <csignal>
 #include <mutex>
#endif

namespace {

// Thrown after the POSIX fault guard longjmps back, to reuse the catch below.
struct PluginFaulted {};

#if ! JUCE_WINDOWS
// ── POSIX in-process plugin fault guard ─────────────────────────────────────
// On Windows, /EHa maps a plugin's structured exception (access violation) onto
// a C++ exception that invokePlugin()'s catch(...) handles. POSIX has no such
// mapping: a faulting in-process plugin raises SIGSEGV/SIGBUS/SIGFPE/SIGILL and,
// uncaught, kills the whole app. These handlers, *only* while a guarded plugin
// call is live on the current thread, siglongjmp() back into invokePlugin() so
// the fault takes the same handled path as Windows — blocklist + leak + survive.
//
// Scope & limits:
//  • The armed flag + landing pad are thread-local with the initial-exec TLS
//    model, so the handler never takes the non-async-signal-safe lazy-TLS path.
//    Only the thread actually inside a plugin call is redirected.
//  • A fault anywhere else (e.g. a V8/GC SIGSEGV trap on a JS thread, or a
//    sanitizer's handler) is chained to the handler we replaced, so we never
//    mask a real crash. Installation happens on the first plugin call, well
//    after V8/Node init, so the chained handler is theirs.
//  • A stack-overflow fault is not reliably caught (no sigaltstack on JUCE's
//    audio threads). The common plugin crash — a bad-pointer dereference — is.
#if defined(__GNUC__) || defined(__clang__)
 #define SC_TLS_IE __attribute__((tls_model("initial-exec")))
#else
 #define SC_TLS_IE
#endif

thread_local SC_TLS_IE sigjmp_buf g_pluginFaultPad;
// volatile sig_atomic_t (not bool): this flag is read+written from the async
// signal handler, where only a volatile sig_atomic_t (or lock-free atomic) is
// well-defined to access. Per-thread (the signal is delivered to the faulting
// thread), initial-exec TLS so the handler never hits lazy-TLS allocation.
thread_local SC_TLS_IE volatile sig_atomic_t g_pluginGuardArmed = 0;

struct SavedSigactions { struct sigaction segv, bus, fpe, ill; };
SavedSigactions g_prevHandlers;
std::once_flag  g_handlerOnce;

const struct sigaction* previousHandlerFor(int sig) noexcept
{
    switch (sig)
    {
        case SIGSEGV: return &g_prevHandlers.segv;
        case SIGBUS:  return &g_prevHandlers.bus;
        case SIGFPE:  return &g_prevHandlers.fpe;
        case SIGILL:  return &g_prevHandlers.ill;
        default:      return nullptr;
    }
}

void pluginFaultHandler(int sig, siginfo_t* info, void* ctx)
{
    if (g_pluginGuardArmed)
    {
        g_pluginGuardArmed = 0;
        siglongjmp(g_pluginFaultPad, sig);   // async-signal-safe; restores mask
    }

    // Not inside a guarded plugin call → a genuine fault elsewhere. Chain to the
    // handler we replaced rather than mask it.
    if (const struct sigaction* prev = previousHandlerFor(sig))
    {
        if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction != nullptr)
        {
            prev->sa_sigaction(sig, info, ctx);
            return;
        }
        if (! (prev->sa_flags & SA_SIGINFO))
        {
            if (prev->sa_handler == SIG_IGN) return;
            if (prev->sa_handler != SIG_DFL && prev->sa_handler != nullptr)
            {
                prev->sa_handler(sig);
                return;
            }
        }
    }
    // Default disposition: restore it and re-raise so the process crashes for real.
    signal(sig, SIG_DFL);
    raise(sig);
}

void installPluginFaultHandlers()
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = pluginFaultHandler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, &g_prevHandlers.segv);
    sigaction(SIGBUS,  &sa, &g_prevHandlers.bus);
    sigaction(SIGFPE,  &sa, &g_prevHandlers.fpe);
    sigaction(SIGILL,  &sa, &g_prevHandlers.ill);
}
#endif // ! JUCE_WINDOWS

// Catch a plugin fault — access violation, heap corruption, C++ exception —
// rather than let it kill the host process. On Windows, /EHa on this TU makes
// catch(...) catch the SEH access violation directly; on POSIX the crash arrives
// as a signal, so we arm the thread-local fault guard above to convert it into
// the same handled path.
//
// On fault: route future loads of the offending plugin through the
// out-of-process sandbox (via the runtime crash blocklist), and *leak* the
// AudioPluginInstance — calling its destructor on a now-corrupted heap is
// its own crash hazard. A one-time leak per kill in exchange for a live app.
// The next iteration of any slot loop sees slot->processor == nullptr and
// skips the slot.
template <typename Fn>
inline void invokePlugin(ProcessorSlot& slot, Fn&& fn) noexcept
{
    if (! slot.processor) return;

#if ! JUCE_WINDOWS
    std::call_once(g_handlerOnce, installPluginFaultHandlers);
    // Captured before the try so the catch can restore it on every exit path.
    // Set before the sigsetjmp and never mutated after, so it is well-defined
    // post-longjmp (the setjmp indeterminate-value rule only bites locals that
    // ARE modified between setjmp and longjmp).
    const sig_atomic_t wasArmed = g_pluginGuardArmed;
#endif

    try
    {
#if ! JUCE_WINDOWS
        // Arm the POSIX crash landing pad around the plugin call. sigsetjmp(.,1)
        // saves the signal mask so the crash signal is unblocked again when the
        // handler longjmps back. A non-zero return means the plugin faulted;
        // route it into the shared catch below (which restores the guard state).
        if (sigsetjmp(g_pluginFaultPad, 1) != 0)
            throw PluginFaulted{};
        g_pluginGuardArmed = 1;
#endif

        fn(*slot.processor);

#if ! JUCE_WINDOWS
        g_pluginGuardArmed = wasArmed;
#endif
    }
    catch (...)
    {
#if ! JUCE_WINDOWS
        // Restore the guard on EVERY exit from the guarded region. A normal C++
        // exception from fn() bypasses the restore above and would otherwise
        // leave this thread armed with a stale landing pad — so a later unrelated
        // SIGSEGV/SIGBUS/… could be misread as a plugin fault and longjmp into a
        // dead frame. (The signal-fault path arrives here too, via PluginFaulted.)
        g_pluginGuardArmed = wasArmed;
#endif
        // Best-effort blocklist update — addCrashedPlugin allocates (juce path
        // canonicalisation, StringArray.add) and locks a mutex, both of which
        // can throw under OOM or corruption. Swallow any exception here so the
        // outer noexcept boundary stays honest; release() is itself noexcept
        // and never escapes the catch.
        try { slopsmith::sandbox::addCrashedPlugin(slot.path); }
        catch (...) { /* nothing useful to do on the noexcept boundary */ }
        (void) slot.processor.release();
    }
}

// Constant-power pan applied to a stereo buffer in place. pan: -1 (L) .. +1 (R);
// 0 = centre = unity on both channels (so the default leaves the signal
// untouched). For the dual-mono amp output this acts as a normal pan-pot; for
// genuinely stereo content it's an equal-power balance.
inline void applyPan(juce::AudioBuffer<float>& buf, int numSamples, float pan) noexcept
{
    if (pan == 0.0f || buf.getNumChannels() < 2 || numSamples <= 0) return;
    pan = juce::jlimit(-1.0f, 1.0f, pan);
    const float theta = (pan + 1.0f) * 0.5f * juce::MathConstants<float>::halfPi; // 0..pi/2
    const float gainL = std::cos(theta) * juce::MathConstants<float>::sqrt2;       // centre -> 1.0
    const float gainR = std::sin(theta) * juce::MathConstants<float>::sqrt2;
    buf.applyGain(0, 0, numSamples, gainL);
    buf.applyGain(1, 0, numSamples, gainR);
}

} // namespace

// ── ProcessorSlot ─────────────────────────────────────────────────────────────

juce::MemoryBlock ProcessorSlot::getState() const
{
    juce::MemoryBlock state;
    if (processor)
        processor->getStateInformation(state);
    return state;
}

void ProcessorSlot::setState(const juce::MemoryBlock& state)
{
    if (processor && state.getSize() > 0)
        processor->setStateInformation(state.getData(), (int)state.getSize());
}

// ── SignalChain ───────────────────────────────────────────────────────────────

SignalChain::SignalChain() {}

SignalChain::~SignalChain()
{
    const juce::ScopedLock sl(lock);
    slots.clear();
}

void SignalChain::prepare(double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;

    // Size the parallel-branch scratch once, off the audio thread. Stereo, the
    // chain's fixed channel layout. avoidReallocating=true keeps the storage
    // stable so the RT path never allocates.
    splitScratch.setSize(2, blockSize, false, false, true);
    branchScratch.setSize(2, blockSize, false, false, true);
    accumScratch.setSize(2, blockSize, false, false, true);

    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
    {
        invokePlugin(*slot, [&](juce::AudioProcessor& p)
        {
            p.releaseResources();
            p.setPlayConfigDetails(2, 2, sampleRate, blockSize);
            p.prepareToPlay(sampleRate, blockSize);
        });
    }
}

void SignalChain::releaseResources()
{
    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
    {
        invokePlugin(*slot, [](juce::AudioProcessor& p)
        {
            p.releaseResources();
        });
    }
}

void SignalChain::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // FTZ/DAZ for the plugin chain — IIR tails decaying to denormals here are a
    // major source of sporadic CPU spikes (see AudioEngine's RT callback note).
    const juce::ScopedNoDenormals noDenormals;

    const juce::ScopedTryLock sl(lock);
    if (!sl.isLocked()) return;

    // Drain pending MIDI messages from the lock-free queue
    struct DrainedMsg { int slotId; juce::MidiMessage msg; };
    DrainedMsg drained[kMidiQueueSize];
    int numDrained = 0;

    const auto scope = midiQueueFifo.read(midiQueueFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1 && numDrained < kMidiQueueSize; ++i)
        drained[numDrained++] = { midiRingBuffer[(size_t)scope.startIndex1 + i].targetSlotId,
                                   midiRingBuffer[(size_t)scope.startIndex1 + i].msg };
    for (int i = 0; i < scope.blockSize2 && numDrained < kMidiQueueSize; ++i)
        drained[numDrained++] = { midiRingBuffer[(size_t)scope.startIndex2 + i].targetSlotId,
                                   midiRingBuffer[(size_t)scope.startIndex2 + i].msg };

    // Reused across slots so the per-slot MIDI buffer isn't heap-allocated on
    // the RT thread every block (it was copy-constructed per slot before).
    juce::MidiBuffer slotMidi;
    const int numSamples = buffer.getNumSamples();

    // Process one slot in place on `buf`: build its MIDI, run it, apply its pan.
    auto runSlot = [&](ProcessorSlot* slot, juce::AudioBuffer<float>& buf)
    {
        if (!slot->processor || slot->bypassed) return;
        slotMidi.clear();
        slotMidi.addEvents(midi, 0, -1, 0);
        for (int i = 0; i < numDrained; ++i)
            if (drained[i].slotId == slot->id || drained[i].slotId == -1)
                slotMidi.addEvent(drained[i].msg, 0);
        invokePlugin(*slot, [&](juce::AudioProcessor& p) { p.processBlock(buf, slotMidi); });
        applyPan(buf, numSamples, slot->pan);
    };

    // Fast path: no parallel branch → plain serial chain. Behaviour is unchanged
    // vs before (pan defaults to 0, so applyPan is a no-op and existing tones are
    // bit-identical).
    bool hasBranch = false;
    for (auto* slot : slots) if (slot->branch != 0) { hasBranch = true; break; }
    if (!hasBranch)
    {
        for (auto* slot : slots) runSlot(slot, buffer);
        return;
    }

    // Parallel path. Slot order (the node editor guarantees it):
    //   [ trunk-pre (branch 0) ][ parallel branches (branch >=1) ][ trunk-post (branch 0) ]
    // Trunk-pre runs in place and its result is the split source every branch
    // reads; each branch runs on its own copy, is panned, and summed into the
    // merge bus; trunk-post then runs on the merged signal. Stereo scratch only —
    // fall back to serial for a non-stereo or oversized block (can't pan/sum).
    if (buffer.getNumChannels() < 2 || numSamples > currentBlockSize)
    {
        for (auto* slot : slots) runSlot(slot, buffer);
        return;
    }
    // Locate the parallel region first, without processing anything yet (this
    // only reads slot->branch): trunk-pre is the leading run of branch==0 slots
    // [0,idx); the branch region is [idx,regionEnd); trunk-post is the rest.
    int idx = 0;
    while (idx < slots.size() && slots[idx]->branch == 0) ++idx;

    int regionEnd = idx, maxBranch = 0;                       // end just past last branch slot
    for (int k = idx; k < slots.size(); ++k)
        if (slots[k]->branch != 0) { regionEnd = k + 1; maxBranch = juce::jmax(maxBranch, slots[k]->branch); }

    // Well-formedness guard: the node editor lays branches out contiguously, so
    // every slot in [idx,regionEnd) must belong to a branch. A stray branch==0
    // (trunk) slot interleaved here would be run by none of the loops below —
    // silent signal loss. If that invariant is ever violated, fall back to a
    // plain serial chain so no slot is dropped. Nothing has been processed in
    // place yet, so the fallback is exact.
    for (int k = idx; k < regionEnd; ++k)
        if (slots[k]->branch == 0)
        {
            jassertfalse;   // malformed branch layout — see comment above
            for (auto* slot : slots) runSlot(slot, buffer);
            return;
        }

    // Match the scratch length to this block (no realloc: capacity == blockSize).
    branchScratch.setSize(2, numSamples, false, false, true);

    for (int k = 0; k < idx; ++k) runSlot(slots[k], buffer); // trunk-pre, in place

    for (int ch = 0; ch < 2; ++ch) splitScratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    accumScratch.clear(0, numSamples);

    for (int b = 1; b <= maxBranch; ++b)
    {
        // Which channel of the split source this branch reads (St-2): 0 = stereo,
        // 1 = L only (→ both), 2 = R only (→ both). From the first slot that sets
        // it; lets a stereo-out gear feed its L to one branch and R to another.
        int bSrc = 0;
        for (int k = idx; k < regionEnd; ++k)
            if (slots[k]->branch == b && slots[k]->branchSrc != 0) { bSrc = slots[k]->branchSrc; break; }
        const int srcL = (bSrc == 2) ? 1 : 0;
        const int srcR = (bSrc == 1) ? 0 : 1;
        bool any = false;
        for (int k = idx; k < regionEnd; ++k)
        {
            if (slots[k]->branch != b) continue;
            if (!any)
            {
                branchScratch.copyFrom(0, 0, splitScratch, srcL, 0, numSamples);
                branchScratch.copyFrom(1, 0, splitScratch, srcR, 0, numSamples);
                any = true;
            }
            runSlot(slots[k], branchScratch);
        }
        if (any)
            for (int ch = 0; ch < 2; ++ch) accumScratch.addFrom(ch, 0, branchScratch, ch, 0, numSamples);
    }

    for (int ch = 0; ch < 2; ++ch) buffer.copyFrom(ch, 0, accumScratch, ch, 0, numSamples);
    for (int k = regionEnd; k < slots.size(); ++k)              // trunk-post on the merged bus
        runSlot(slots[k], buffer);
}

void SignalChain::queueMidiMessage(int targetSlotId, const juce::MidiMessage& msg)
{
    const auto scope = midiQueueFifo.write(1);
    if (scope.blockSize1 > 0)
        midiRingBuffer[(size_t)scope.startIndex1] = { targetSlotId, msg };
    else if (scope.blockSize2 > 0)
        midiRingBuffer[(size_t)scope.startIndex2] = { targetSlotId, msg };
    // If queue full, message silently dropped (acceptable for PC messages)
}

int SignalChain::addProcessor(std::unique_ptr<juce::AudioProcessor> processor,
                               ProcessorSlot::Type type,
                               const juce::String& name,
                               const juce::String& path)
{
    if (!processor) return -1;

    auto slot = std::make_unique<ProcessorSlot>();
    slot->type = type;
    slot->processor = std::move(processor);
    slot->name = name;
    slot->path = path;
    slot->id = nextSlotId++;

    // Prepare under the SEH-catching helper so a plugin that faults during
    // prepareToPlay is blocklisted (next load routes to the sandbox) and the
    // slot is dropped, rather than taking the app down.
    invokePlugin(*slot, [&](juce::AudioProcessor& p)
    {
        p.setPlayConfigDetails(2, 2, currentSampleRate, currentBlockSize);
        p.prepareToPlay(currentSampleRate, currentBlockSize);
    });
    if (! slot->processor) return -1;

    int id = slot->id;
    const juce::ScopedLock sl(lock);
    slots.add(slot.release());
    return id;
}

void SignalChain::removeProcessor(int slotId)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots.remove(idx);
}

void SignalChain::moveProcessor(int fromIndex, int toIndex)
{
    const juce::ScopedLock sl(lock);
    if (fromIndex >= 0 && fromIndex < slots.size() &&
        toIndex >= 0 && toIndex < slots.size() && fromIndex != toIndex)
    {
        slots.move(fromIndex, toIndex);
    }
}

void SignalChain::setBypass(int slotId, bool bypassed)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots[idx]->bypassed = bypassed;
}

void SignalChain::setMultiBypass(const juce::Array<std::pair<int, bool>>& changes)
{
    const juce::ScopedLock sl(lock);
    for (auto& [slotId, bypassed] : changes)
    {
        int idx = findSlotIndex(slotId);
        if (idx >= 0) slots[idx]->bypassed = bypassed;
    }
}

void SignalChain::setPan(int slotId, float pan)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots[idx]->pan = juce::jlimit(-1.0f, 1.0f, pan);
}

void SignalChain::setBranch(int slotId, int branch)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots[idx]->branch = juce::jmax(0, branch);
}

void SignalChain::setBranchSrc(int slotId, int src)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0) slots[idx]->branchSrc = juce::jlimit(0, 2, src);
}

void SignalChain::clear()
{
    // Detach the slots under a BRIEF lock, then destroy them OFF the lock. The
    // destructors tear down sandbox subprocesses (IPC + waits) which is slow;
    // doing that while holding `lock` starved the RT process() ScopedTryLock and
    // dropped audio blocks → the "scratches" heard whenever a chain reloads.
    juce::OwnedArray<ProcessorSlot> dead;
    { const juce::ScopedLock sl(lock); slots.swapWith(dead); }
    dead.clear();
}

int SignalChain::getNumSlots() const
{
    const juce::ScopedLock sl(lock);
    return slots.size();
}

const ProcessorSlot* SignalChain::getSlot(int slotId) const
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    return idx >= 0 ? slots[idx] : nullptr;
}

juce::Array<const ProcessorSlot*> SignalChain::getAllSlots() const
{
    juce::Array<const ProcessorSlot*> result;
    const juce::ScopedLock sl(lock);
    for (auto* slot : slots)
        result.add(slot);
    return result;
}

juce::Array<SignalChain::ParamInfo> SignalChain::getParameters(int slotId) const
{
    juce::Array<ParamInfo> result;
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx < 0) return result;

    auto* proc = slots[idx]->processor.get();
    if (!proc) return result;

    auto& params = proc->getParameters();
    for (int i = 0; i < params.size(); ++i)
    {
        ParamInfo info;
        info.index = i;
        info.name = params[i]->getName(128);
        info.value = params[i]->getValue();
        info.label = params[i]->getLabel();
        info.text = params[i]->getCurrentValueAsText();
        result.add(info);
    }
    return result;
}

void SignalChain::setParameter(int slotId, int paramIndex, float value)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx < 0) return;

    auto* proc = slots[idx]->processor.get();
    if (!proc) return;

    // Out-of-process VSTs expose no JUCE parameter proxies, so forward the change
    // over the control pipe — otherwise knob/preset automation never reaches the
    // sandboxed plugin and tones play at their defaults.
    if (auto* sp = dynamic_cast<slopsmith::sandbox::SandboxedProcessor*>(proc))
    {
        sp->setSandboxedParameter(paramIndex, value);
        return;
    }

    auto& params = proc->getParameters();
    if (paramIndex >= 0 && paramIndex < params.size())
        params[paramIndex]->setValue(value);
}

void SignalChain::setSlotState(int slotId, const juce::MemoryBlock& state)
{
    const juce::ScopedLock sl(lock);
    int idx = findSlotIndex(slotId);
    if (idx >= 0)
        slots[idx]->setState(state); // ProcessorSlot::setState() is null/empty-safe
}

// ── Presets ───────────────────────────────────────────────────────────────────

juce::String SignalChain::savePreset() const
{
    auto root = new juce::DynamicObject();
    root->setProperty("version", 1);

    juce::Array<juce::var> chainArray;
    const juce::ScopedLock sl(lock);

    for (auto* slot : slots)
    {
        auto slotObj = new juce::DynamicObject();
        slotObj->setProperty("id", slot->id);
        slotObj->setProperty("type", (int)slot->type);
        slotObj->setProperty("name", slot->name);
        slotObj->setProperty("path", slot->path);
        slotObj->setProperty("bypassed", slot->bypassed);

        // Stereo routing (St-1) — only emitted when non-default so existing mono
        // presets are byte-for-byte unchanged.
        if (slot->pan != 0.0f)       slotObj->setProperty("pan", slot->pan);
        if (slot->branch != 0)       slotObj->setProperty("branch", slot->branch);
        if (slot->branchSrc != 0)    slotObj->setProperty("branchSrc", slot->branchSrc);

        // Save processor state as base64
        auto state = slot->getState();
        if (state.getSize() > 0)
            slotObj->setProperty("state", state.toBase64Encoding());

        chainArray.add(juce::var(slotObj));
    }

    root->setProperty("chain", juce::var(chainArray));
    return juce::JSON::toString(juce::var(root));
}

void SignalChain::loadPreset(const juce::String& json)
{
    // Preset loading is handled at a higher level (NodeAddon) because
    // it needs to re-instantiate processors (VSTs, NAMs, IRs) which
    // requires the VSTHost and other components. The chain just needs
    // to be rebuilt via addProcessor() calls followed by setState().
}

// ── Private ───────────────────────────────────────────────────────────────────

int SignalChain::findSlotIndex(int slotId) const
{
    for (int i = 0; i < slots.size(); ++i)
        if (slots[i]->id == slotId) return i;
    return -1;
}
