#pragma once

// LifecycleExecutor — P0 of the audio architecture guide (§12 P0).
//
// One serialized queue for native lifecycle mutations (engine init/shutdown,
// device create/destroy, editor teardown). Ops are named, stamped with the
// engine generation at submit time, and executed strictly in submission
// order on the JUCE message thread.
//
// What this replaces: the "dispatch and give up after 15 s" contract
// (AddonContext.h, KNOWN LIMITATION). Field evidence 2026-07-17 (three
// identical CFG fail-fast dumps): a caller that abandons its wait proceeds
// to mutate lifecycle state while the abandoned op is still queued — two
// owners, no ordering authority. The executor removes the abandon path:
//
//   - The wait is not bounded by a give-up. A watchdog logs every
//     kWatchdogIntervalMs while an op is stalled (message thread blocked by
//     a driver call) and keeps waiting. The op result the caller sees is
//     always the truth of what ran.
//   - The only false returns are "the op will never run" (message pump gone
//     or post refused) and "the op was stale" (engine generation changed
//     between submit and execution — see below). Callers can treat false as
//     final; there is no "maybe it still runs later" state.
//   - `maxWaitMs` exists for the one caller that must not block forever:
//     process shutdown, where leaking the pump beats hanging exit. Everyone
//     else passes 0 (unbounded).
//
// Generation guard: initialize/doShutdown bump the engine generation. An op
// stamped under an older generation no-ops when it finally executes, so
// work that was queued against a torn-down engine can never touch its
// replacement. This is the lifecycle-level generalization of
// chainGeneration.
//
// The executor serializes; it never authorizes. Policy (who may mutate
// what) belongs to the JS-side lease registry (guide §7.1).
//
// macOS: no background pump exists (AppKit owns the real main thread —
// AddonContext.cpp fork note), so ops execute inline on the caller thread,
// still generation-checked. Same contract, degenerate queue.

#include <cstdint>
#include <functional>

namespace slopsmith::addon {

// Monotonic engine generation. Bumped by initialize()/doShutdown().
std::uint64_t currentEngineGeneration();
std::uint64_t bumpEngineGeneration();

enum class LifecycleOpResult {
    completed,      // op ran to completion under its submit-time generation
    stale,          // generation changed before the op ran; op no-oped
    neverRan,       // pump gone / post refused — op will never run
    abandonedWait,  // maxWaitMs expired; op may still run (shutdown-only path)
};

// Run `func` on the JUCE message thread, serialized behind every previously
// submitted lifecycle op. Blocks until the op completes (watchdog logs
// while stalled). `name` appears in every log line and in the watchdog.
// `maxWaitMs` = 0 means wait indefinitely (bail only if the pump dies).
LifecycleOpResult runLifecycleOp(const char* name,
                                 std::function<void()> func,
                                 int maxWaitMs = 0);

// Convenience: true only for LifecycleOpResult::completed.
inline bool runLifecycleOpOk(const char* name, std::function<void()> func,
                             int maxWaitMs = 0)
{
    return runLifecycleOp(name, std::move(func), maxWaitMs)
           == LifecycleOpResult::completed;
}

// Plugin-module pinning (guide §12 P0 item 4) lives in PluginModulePin.h —
// header-only, because VSTHost.cpp is compiled into targets across three
// CMake projects and an out-of-line definition broke the ones that don't
// build this executor (PR #120 CI).

} // namespace slopsmith::addon
