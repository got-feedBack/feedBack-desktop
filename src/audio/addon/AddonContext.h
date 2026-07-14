#pragma once

// AddonContext — engine/vstHost lifetime, the JUCE message thread, the
// shutdown latch, and the pending-async-load registry (TLC plan phase 6 /
// §3.1). Moved verbatim from NodeAddon.cpp; this quarantines the JUCE_MAC
// platform fork (no dispatch loop — see startJuceMessageThread) into ONE
// file instead of a branch inside every load path.
//
// engine / vstHost — shared_ptr (not unique_ptr) so worker threads can take
// a stable snapshot that keeps the object alive for the duration of their
// work, even if the message thread reassigns the global mid-operation. This
// matters most for the async VST load: createPluginInstanceAsync's JUCE
// continuation must not have VSTHost / its formatManager torn out from
// under it mid-load.
//
// Enforced rule: every *dereference* of engine / vstHost goes through a
// local snapshot (snapshotEngine / snapshotVstHost). The only code touching
// the bare globals is the snapshot helpers and the mutex-guarded writes in
// initialize / doShutdown.

#include "../AudioEngine.h"
#include "../VSTHost.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace slopsmith::addon {

std::shared_ptr<AudioEngine> snapshotEngine();
std::shared_ptr<VSTHost> snapshotVstHost();

// Start the pump + create engine/vstHost on the message thread (inline on
// macOS). `uiTeardownHook` runs on the message thread at the START of
// shutdown, BEFORE engine.reset() frees the processors — NodeAddon points it
// at destroyAllPluginEditorWindowsOnMessageThread (use-after-free; #56).
void initialize(std::function<void()> uiTeardownHook);
void doShutdown();

// Dispatch `func` on the JUCE message thread and wait (bounded 15 s).
// macOS: executes inline on the caller thread — no background pump exists
// (AppKit owns the real main thread; see the fork note in the .cpp).
// Returns false when the work did not complete: the post was refused
// (message queue gone — `func` will never run) or the wait timed out
// (`func` may still run later). Lifecycle callers must treat false as
// "teardown/init did not happen" rather than continuing.
bool dispatchOnMessageThreadImpl(std::function<void()> func);
template <typename Func>
inline bool dispatchOnMessageThread(Func&& func)
{
    return dispatchOnMessageThreadImpl(std::function<void()>(std::forward<Func>(func)));
}

// Pending-async-load registry: LoadVSTWorker / LoadPresetWorker block on a
// WaitableEvent until the message-thread continuation fires; doShutdown
// signals every registered event so no worker waits forever once the pump
// is gone.
// Whether doShutdown has begun (acquire). The load workers gate on this
// after registering their pending event, catching the register-vs-shutdown
// race in both directions.
bool isShuttingDown();

void registerPendingLoad(std::shared_ptr<juce::WaitableEvent> evt);
void unregisterPendingLoad(const std::shared_ptr<juce::WaitableEvent>& evt);
void cancelAllPendingLoads();

} // namespace slopsmith::addon
