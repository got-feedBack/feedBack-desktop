// AudioChannel — platform-neutral lock-free ring logic.
//
// The shared-memory layout (Protocol.h / AudioShmHeader), the producer/
// consumer index discipline, and the inline-MIDI packing are identical on
// every OS. Only the shared-memory create/open/close and the doorbell
// signal/wait differ, and those live behind AudioChannel::Impl::signalEvent /
// waitEvent + the create/open/close trio in AudioChannel_{win,posix}.cpp.
//
// Threading: see AudioChannel.h. The release-store on a write index pairs with
// the consumer's acquire-load of the same index, which is what makes the plain
// memcpy/memset slot writes visible to the consumer — release/acquire on the
// shared atomic establishes happens-before regardless of architecture
// (x86/x64 and arm64 alike), and across the process boundary because both
// sides map the same physical memory.

#include "AudioChannelImpl.h"
#include "../VSTTrace.h"

#include <atomic>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
 #include <emmintrin.h>   // _mm_pause for the cpuRelax() spin hint below
#endif

namespace slopsmith::sandbox {

// CPU "relax" hint for short bounded spins: yields the pipeline to a
// hyper-threaded sibling and lowers power vs. a bare load loop. No effect on
// correctness — purely a spin-politeness hint, no-op where unavailable.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
 static inline void cpuRelax() noexcept { _mm_pause(); }
#elif defined(__aarch64__) || defined(__arm__)
 static inline void cpuRelax() noexcept { __asm__ __volatile__("yield" ::: "memory"); }
#else
 static inline void cpuRelax() noexcept {}
#endif

AudioChannel::AudioChannel() : impl(std::make_unique<Impl>()) {}

AudioChannel::~AudioChannel() { close(); }

// Atomic access to the plain uint64_t/uint32_t shm header indices (written by
// one side, read by the other across the process boundary). We avoid UB from
// reinterpret_cast'ing the storage to std::atomic<T>* (not layout-guaranteed)
// in one of two ways:
//
//   * std::atomic_ref<T> (C++20, P0019) where the library provides it — MSVC,
//     libstdc++ 11+, libc++ 19+ (Apple clang / Xcode 16+).
//   * the gcc/clang __atomic builtins otherwise — notably older Apple libc++
//     (pre-Xcode-16 macOS runners) which lacks std::atomic_ref. Same lock-free
//     acquire/release semantics on the caller-aligned object. This branch uses
//     gcc/clang-only builtins; it is never compiled on MSVC, which always has
//     std::atomic_ref. (SLOPSMITH_FORCE_ATOMIC_BUILTINS forces it for testing.)
#if defined(__cpp_lib_atomic_ref) && ! defined(SLOPSMITH_FORCE_ATOMIC_BUILTINS)
// `atomic_ref<T>::required_alignment` may exceed `alignof(T)`; header fields
// are `alignas(8)`. Fail at compile time if a platform needs more, rather than
// constructing an atomic_ref on an under-aligned object (UB).
static_assert(std::atomic_ref<uint64_t>::required_alignment <= 8,
              "AudioShmHeader uint64_t fields are alignas(8); bump the "
              "alignas to std::atomic_ref<uint64_t>::required_alignment");
template <typename T>
static std::atomic_ref<T> atomicRefOf(T& slot) { return std::atomic_ref<T>(slot); }
#else
// std::memory_order's enumerator values match the __ATOMIC_* constants on
// gcc/clang, so the cast is exact.
template <typename T>
struct BuiltinAtomicRef
{
    T* p;
    explicit BuiltinAtomicRef(T& r) : p(&r) {}
    T    load(std::memory_order o) const { return __atomic_load_n(p, static_cast<int>(o)); }
    void store(T v, std::memory_order o) { __atomic_store_n(p, v, static_cast<int>(o)); }
    T    fetch_add(T v, std::memory_order o) { return __atomic_fetch_add(p, v, static_cast<int>(o)); }
};
template <typename T>
static BuiltinAtomicRef<T> atomicRefOf(T& slot) { return BuiltinAtomicRef<T>(slot); }
#endif

static auto atomicAt(uint64_t& slot)   { return atomicRefOf(slot); }
static auto atomicAt32(uint32_t& slot) { return atomicRefOf(slot); }

namespace
{
    // Pick the right (write, read) index pair for a direction. Input ring
    // (host → sandbox) is produced by host / consumed by sandbox; output
    // ring (sandbox → host) is the inverse.
    struct RingIndices { uint64_t& write; uint64_t& read; };

    RingIndices indicesFor(AudioShmHeader& h, bool isOutputRing)
    {
        return isOutputRing
             ? RingIndices{ h.outWriteIdx, h.outReadIdx }
             : RingIndices{ h.inWriteIdx,  h.inReadIdx  };
    }
}

bool AudioChannel::pushBlock(bool isOutputRing, const juce::AudioBuffer<float>& src,
                             int numSamples)
{
    // Input-direction pushes MUST go through pushInputBlock (which publishes
    // the slot's MidiQueue alongside the audio). Calling pushBlock(false,…)
    // directly would leave whatever MIDI count was in the slot from a prior
    // pushInputBlock and the next popInputBlock would replay those stale
    // events against fresh audio.
    //
    // jassert in debug + return false in release: a release-build regression
    // would otherwise silently corrupt MIDI delivery rather than failing
    // loudly. Today the only input producer is
    // SandboxedProcessor::processBlock and it always calls pushInputBlock.
    jassert(isOutputRing);
    if (!isOutputRing) return false;
    if (!impl->header) return false;
    auto idx = indicesFor(*impl->header, isOutputRing);
    auto writeIdx = atomicAt(idx.write);
    auto readIdx  = atomicAt(idx.read);
    uint64_t w = writeIdx.load(std::memory_order_relaxed);
    uint64_t r = readIdx.load(std::memory_order_acquire);
    if (w - r >= impl->header->maxBlocks)
    {
        atomicAt(impl->header->xruns).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto slot = w % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* dst = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    const int maxCh      = (int)impl->header->maxChannels;
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int channels   = juce::jmin(maxCh, src.getNumChannels());
    const int samples    = juce::jmin(maxSamples, numSamples);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* slotCh = dst + ch * maxSamples;
        std::memcpy(slotCh, src.getReadPointer(ch),
                    sizeof(float) * (size_t)samples);
        // Wipe tail samples so a shorter block doesn't leave audio from a
        // previous slot-overwrite hanging around for the consumer.
        if (samples < maxSamples)
            std::memset(slotCh + samples, 0,
                        sizeof(float) * (size_t)(maxSamples - samples));
    }
    // Wipe channels the producer didn't write at all — same rationale.
    for (int ch = channels; ch < maxCh; ++ch)
        std::memset(dst + ch * maxSamples, 0,
                    sizeof(float) * (size_t)maxSamples);

    // The release store on writeIdx pairs with the consumer's acquire load in
    // popBlock, so the slot memcpy/memset above are happens-before any read of
    // writeIdx >= w+1 — on every supported architecture (the pairing, not the
    // hardware, is what guarantees it). signalEvent is only a wakeup; the
    // actual data handoff is the index + shm.
    writeIdx.store(w + 1, std::memory_order_release);
    impl->signalEvent(isOutputRing);
    return true;
}

bool AudioChannel::popBlock(bool isOutputRing, juce::AudioBuffer<float>& dst,
                            int numSamples, int timeoutMs)
{
    if (!impl->header) return false;
    auto idx = indicesFor(*impl->header, isOutputRing);
    auto writeIdx = atomicAt(idx.write);
    auto readIdx  = atomicAt(idx.read);

    // Check indices BEFORE waiting. The doorbell is not counting: if the
    // producer signals twice in a row (queue 2 blocks), the wakes collapse
    // (Win32 auto-reset event) or are drained together (POSIX socketpair).
    // Without this fast path, the consumer would block on the second pop even
    // though w > r already.
    uint64_t r = readIdx.load(std::memory_order_relaxed);
    uint64_t w = writeIdx.load(std::memory_order_acquire);
    if (w == r)
    {
        // Bounded busy-spin before the blocking wait: a fast plugin lands its
        // output a few microseconds after we checked, so spinning on the write
        // index catches the common case without paying the poll() syscall + the
        // cross-process doorbell wakeup latency — which, multiplied across an
        // N-plugin chain, is a big slice of the per-block budget. A slow plugin
        // exits the (short) spin still empty and falls through to the efficient
        // blocking wait, so correctness and CPU cost for heavy chains are unchanged.
        constexpr int kPopSpinIters = 2000;
        for (int s = 0; s < kPopSpinIters; ++s)
        {
            w = writeIdx.load(std::memory_order_acquire);
            if (w != r) break;
            cpuRelax();   // don't starve the HT sibling / burn power while spinning
        }
        if (w == r && !impl->waitEvent(isOutputRing, timeoutMs))
        {
            atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Re-read; the wake might have been from teardown, an
        // AudioPauseGuard's signalSandboxWake (every kPrepare /
        // kSetBlockSize / kGetState / kSetState), or a kShutdown /
        // disconnect callback. NONE of those are dropouts — they're
        // intentional non-data wakes. Don't bump `dropouts` here or the
        // counter pollutes every pause-guarded control op. Real
        // dropouts are still counted on the waitEvent timeout path above
        // and at the SandboxedProcessor pop-timeout call site.
        r = readIdx.load(std::memory_order_relaxed);
        w = writeIdx.load(std::memory_order_acquire);
        if (w == r) return false;
    }

    auto slot = r % impl->header->maxBlocks;
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = (isOutputRing ? impl->outputRing : impl->inputRing)
              + slot * (bytesPerSlot / sizeof(float));

    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int dstCh      = dst.getNumChannels();
    const int channels   = juce::jmin((int)impl->header->maxChannels, dstCh);
    const int samples    = juce::jmin(maxSamples, numSamples);
    if (numSamples > maxSamples)
    {
        // One-shot warn: caller passed more samples than the spawn-time cap
        // allows, so we'll truncate to maxSamples and zero-fill the tail.
        // Producer-side push paths apply the same clamp, so this fires only
        // if a misconfigured consumer asks for too much (kPrepare /
        // kSetBlockSize spawn-cap validation should have prevented it).
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel))
        {
            VST_TRACE("[audio-shm] popBlock: caller numSamples=%d > spawn cap "
                      "maxBlockSamples=%d — truncating, tail zeroed",
                      numSamples, maxSamples);
        }
    }
    for (int ch = 0; ch < channels; ++ch)
    {
        std::memcpy(dst.getWritePointer(ch),
                    src + ch * maxSamples,
                    sizeof(float) * (size_t)samples);
        // Zero any portion of dst beyond what we copied (the caller's buffer
        // may be longer than the producer's payload).
        if (samples < numSamples)
            std::memset(dst.getWritePointer(ch) + samples, 0,
                        sizeof(float) * (size_t)(numSamples - samples));
    }
    // Zero channels the producer didn't fill so dst doesn't carry stale audio.
    for (int ch = channels; ch < dstCh; ++ch)
        dst.clear(ch, 0, numSamples);

    readIdx.store(r + 1, std::memory_order_release);
    return true;
}

bool AudioChannel::pushInputBlock(const juce::AudioBuffer<float>& src,
                                  const juce::MidiBuffer& midi,
                                  int numSamples)
{
    // Inlined audio + MIDI publish so the slot's MidiQueue is published
    // alongside the audio under the same inWriteIdx release. Earlier this
    // method delegated to pushBlock(false, ...) for the audio half, but
    // pushBlock used to clobber `count` to 0 between our MIDI publish and
    // the inWriteIdx bump — every MIDI event was being dropped.
    if (!impl->header || !impl->midiQueues) return false;
    // Reject up front when the caller exceeds the spawn-time cap rather
    // than silently truncating audio + dropping MIDI in [maxSamples,
    // numSamples) into midiOverflows. Spawn-cap validation in kPrepare /
    // kSetBlockSize should prevent this; if it ever fires the caller
    // gets a `false` return — that's the diagnostic. Don't bump dropouts
    // (it means "real audio dropout / missed deadline") or xruns (means
    // "destination ring was full"); caller misuse is its own class and
    // conflating them muddles operator-facing metrics.
    //
    // jassert in debug + return false in release — same fail-fast pattern
    // as pushBlock(isOutputRing). Today the only producer
    // (SandboxedProcessor::processBlock) bounds numSamples to JUCE's
    // negotiated block size, so this branch is unreachable in practice;
    // the assert flags any future caller that introduces a path where it
    // becomes reachable.
    jassert(numSamples <= (int)impl->header->maxBlockSamples);
    if (numSamples > (int)impl->header->maxBlockSamples)
        return false;

    auto writeIdx = atomicAt(impl->header->inWriteIdx);
    auto readIdx  = atomicAt(impl->header->inReadIdx);
    uint64_t w = writeIdx.load(std::memory_order_relaxed);
    uint64_t r = readIdx.load(std::memory_order_acquire);
    if (w - r >= impl->header->maxBlocks)
    {
        atomicAt(impl->header->xruns).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto slot = w % impl->header->maxBlocks;
    auto& queue = impl->midiQueues[slot];

    // Compute the truncated sample count up front so the MIDI loop below
    // can clamp event frames against the SAME bound the audio copy uses.
    // If the caller passed numSamples > maxSamples, both halves truncate
    // to maxSamples consistently — otherwise the sandbox would receive
    // MIDI frames pointing past the end of the audio it actually got.
    const int maxCh      = (int)impl->header->maxChannels;
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int channels   = juce::jmin(maxCh, src.getNumChannels());
    const int samples    = juce::jmin(maxSamples, numSamples);

    // 1. Pack MIDI into the slot's queue. The slot is owned by the host
    //    until we publish the new inWriteIdx below, so writes here are
    //    private — no need for the relaxed-clear-then-release-store on
    //    `count`, the release on inWriteIdx publishes both `count` and
    //    `events[]` together. Using atomic_ref for the count store anyway
    //    so the layout stays consistent for the sandbox-side acquire load.
    uint32_t written = 0;
    auto bumpMidiOverflow = [&](uint64_t n = 1)
    {
        // Global cumulative counter — per-slot was confusing because slots
        // round-robin (the per-slot value would mix counts from many
        // different blocks rather than answering "did THIS block
        // overflow?"). Per-event accuracy past the cap is not a documented
        // contract — the bulk-bump on cap-overflow keeps the audio thread
        // from iterating arbitrarily many events on a real-time path.
        atomicAt(impl->header->midiOverflows).fetch_add(n, std::memory_order_relaxed);
    };
    int scanned = 0;
    const int totalEvents = midi.getNumEvents();
    // Hard cap on iterations regardless of accept/reject ratio. The
    // cap-overflow break below bounds the loop when events are valid-and-
    // fit (written climbs to kMidiEventsPerSlot quickly), but a flood of
    // pure SysEx would never increment `written` and could otherwise
    // iterate the entire buffer one event at a time on the RT thread.
    // 2× kMidiEventsPerSlot leaves headroom for normal mixed-in
    // SysEx-among-CCs blocks while still bounding the worst case.
    constexpr int kMaxScanIterations = 2 * (int)kMidiEventsPerSlot;
    for (const auto meta : midi)
    {
        if (scanned >= kMaxScanIterations)
        {
            // Hit the per-block scan cap. Bulk-bump remaining and break;
            // per-event accuracy past the cap is not a documented
            // contract, the bound matters more on the audio thread.
            bumpMidiOverflow((uint64_t)(totalEvents - scanned));
            break;
        }
        ++scanned;
        const auto& msg = meta.getMessage();
        const int rawSize = msg.getRawDataSize();
        if (rawSize <= 0 || rawSize > (int)kMidiEventMaxBytes)
        {
            // Doesn't fit (SysEx etc.). Audio thread never blocks; the
            // lossy policy is documented in PR #2.
            bumpMidiOverflow();
            continue;
        }
        if (written >= kMidiEventsPerSlot)
        {
            // Bulk-bump for THIS event + every remaining event the
            // iterator would visit, then break. Together with the
            // scan-cap above, total audio-thread MIDI work is bounded
            // at 2× kMidiEventsPerSlot iterations regardless of how
            // bloated or pathological the inbound buffer is.
            bumpMidiOverflow((uint64_t)(totalEvents - scanned + 1));
            break;
        }
        // Reject events whose frame is past the truncated audio (samples
        // ≤ numSamples — see the comment on the maxSamples computation
        // above). Clamping would silently re-time the event into the
        // audible portion, which is a worse failure mode than dropping it.
        // samplePosition < 0 is an invalid input; treat it as out-of-range
        // and drop too.
        if (meta.samplePosition < 0 || meta.samplePosition >= samples)
        {
            bumpMidiOverflow();
            continue;
        }
        auto& ev = queue.events[written];
        ev.frame = (uint32_t)meta.samplePosition;
        ev.size  = (uint32_t)rawSize;
        std::memcpy(ev.bytes, msg.getRawData(), (size_t)rawSize);
        ++written;
    }
    // Relaxed: the inWriteIdx release-store below synchronises this write
    // with the sandbox's acquire-load of inWriteIdx in popInputBlock, so
    // when the consumer observes the new write index it also observes
    // count + events[].
    atomicAt32(queue.count).store(written, std::memory_order_relaxed);

    // 2. Copy audio into the same slot of the input ring.
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* dst = impl->inputRing + slot * (bytesPerSlot / sizeof(float));
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* slotCh = dst + ch * maxSamples;
        std::memcpy(slotCh, src.getReadPointer(ch),
                    sizeof(float) * (size_t)samples);
        if (samples < maxSamples)
            std::memset(slotCh + samples, 0,
                        sizeof(float) * (size_t)(maxSamples - samples));
    }
    for (int ch = channels; ch < maxCh; ++ch)
        std::memset(dst + ch * maxSamples, 0,
                    sizeof(float) * (size_t)maxSamples);

    // 3. Publish the slot — release-synchronises with the consumer's acquire
    //    on inWriteIdx in popInputBlock, which makes both the audio bytes
    //    and the MIDI queue visible together.
    writeIdx.store(w + 1, std::memory_order_release);
    impl->signalEvent(/*isOutputRing*/ false);
    return true;
}

bool AudioChannel::popInputBlock(juce::AudioBuffer<float>& dst,
                                 juce::MidiBuffer& midi,
                                 int numSamples, int timeoutMs)
{
    // Inlined audio + MIDI drain so we hold the slot until both are read.
    // Earlier this method delegated to popBlock(false, ...), which advanced
    // inReadIdx before the MIDI was drained — the host could then immediately
    // reuse the slot and overwrite the queue we were still reading.
    if (!impl->header || !impl->midiQueues) return false;
    // Symmetric with pushInputBlock: reject up front when the caller
    // exceeds the spawn-time cap, so a misconfigured consumer learns
    // about the misuse via the false return rather than getting silently
    // truncated audio. Don't advance inReadIdx — the producer's slot
    // stays full until the consumer corrects its numSamples (or the host
    // tears down). Don't bump dropouts — caller misuse is its own class;
    // see the matching comment in pushInputBlock.
    //
    // jassert + return false: today's only consumer (runAudioThread
    // calls with currentBlockSize = jlimit(1, bufferCap, ...)) makes
    // this branch unreachable; the assert flags any future caller that
    // changes that.
    jassert(numSamples <= (int)impl->header->maxBlockSamples);
    if (numSamples > (int)impl->header->maxBlockSamples)
        return false;

    auto writeIdx = atomicAt(impl->header->inWriteIdx);
    auto readIdx  = atomicAt(impl->header->inReadIdx);

    // Same fast-path / wait / recheck pattern as popBlock: the doorbell
    // collapses/coalesces signals, so if pushInputBlock fires twice in a row
    // we'd otherwise block on the second pop even though w > r.
    uint64_t r = readIdx.load(std::memory_order_relaxed);
    uint64_t w = writeIdx.load(std::memory_order_acquire);
    if (w == r)
    {
        if (!impl->waitEvent(/*isOutputRing*/ false, timeoutMs))
        {
            atomicAt(impl->header->dropouts).fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        r = readIdx.load(std::memory_order_relaxed);
        w = writeIdx.load(std::memory_order_acquire);
        // Intentional non-data wake (AudioPauseGuard signalSandboxWake on
        // every pause-guarded control op, kShutdown, disconnect). Same
        // rationale as popBlock: don't count these as dropouts. Real
        // missed-deadline events are caught by the timeout branch above
        // and by SandboxedProcessor's pop-timeout call site.
        if (w == r) return false;
    }

    const auto slot = r % impl->header->maxBlocks;

    // 1. Drain MIDI from the slot. Relaxed-load on `count` is sufficient:
    //    the synchronisation that publishes count + events[] is the
    //    acquire-load on inWriteIdx above (paired with the producer's
    //    release-store on inWriteIdx in pushInputBlock), and the producer
    //    writes count itself with relaxed semantics. The acquire here
    //    would be redundant overhead and slightly misleading about the
    //    actual sync model.
    auto& queue = impl->midiQueues[slot];
    const uint32_t count = atomicAt32(queue.count)
                                .load(std::memory_order_relaxed);
    const uint32_t safeCount = juce::jmin(count, kMidiEventsPerSlot);
    for (uint32_t i = 0; i < safeCount; ++i)
    {
        const auto& ev = queue.events[i];
        const uint32_t size = juce::jmin(ev.size, kMidiEventMaxBytes);
        if (size == 0) continue;
        midi.addEvent(juce::MidiMessage(ev.bytes, (int)size),
                      (int)ev.frame);
    }

    // 2. Copy audio out of the slot.
    auto bytesPerSlot = impl->header->ringBytesPerSlot;
    auto* src = impl->inputRing + slot * (bytesPerSlot / sizeof(float));
    // numSamples ≤ maxSamples here (cap enforced by the early-return guard
    // at the top of this function), so samples == numSamples and no
    // tail-zero / one-shot warn is needed — both belonged to the old
    // truncate-and-continue path.
    const int maxSamples = (int)impl->header->maxBlockSamples;
    const int dstCh      = dst.getNumChannels();
    const int channels   = juce::jmin((int)impl->header->maxChannels, dstCh);
    for (int ch = 0; ch < channels; ++ch)
        std::memcpy(dst.getWritePointer(ch),
                    src + ch * maxSamples,
                    sizeof(float) * (size_t)numSamples);
    for (int ch = channels; ch < dstCh; ++ch)
        dst.clear(ch, 0, numSamples);

    // 3. Release the slot — the host can now reuse it; we've finished both
    //    audio and MIDI reads.
    readIdx.store(r + 1, std::memory_order_release);
    return true;
}

uint64_t AudioChannel::diagMidiOverflows() const noexcept
{
    if (!impl->header) return 0;
    return atomicAt(impl->header->midiOverflows).load(std::memory_order_relaxed);
}

uint64_t AudioChannel::diagXruns() const noexcept
{
    if (!impl->header) return 0;
    return atomicAt(impl->header->xruns).load(std::memory_order_relaxed);
}

uint64_t AudioChannel::diagDropouts() const noexcept
{
    if (!impl->header) return 0;
    return atomicAt(impl->header->dropouts).load(std::memory_order_relaxed);
}

void AudioChannel::signalSandboxWake()
{
    // Wake the sandbox audio worker out of its popInputBlock wait without
    // publishing a real block — the input-ring doorbell. Used by the
    // sandbox-side AudioPauseGuard around non-realtime control ops + shutdown.
    // This must wake OUR OWN worker (same process), so it goes through wakeSelf
    // (POSIX self-pipe / Win32 evtToSandbox) — NOT signalEvent, whose POSIX
    // socketpair write would hit the peer instead and spuriously wake it.
    impl->wakeSelf();
}

} // namespace slopsmith::sandbox
