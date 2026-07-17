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
#include "LifecycleExecutor.h"

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

// Run a device-lifecycle mutation (anything that can create or destroy a
// juce::AudioIODevice: setAudioDevices, start/stopAudio, device-type
// switches, stream-output open/close, extra-input add/remove) on the JUCE
// message thread.
//
// Why: on Windows, ASIO devices arm juce::Timers (the driver's deferred
// kAsioResetRequest, device-change detection) that fire on the message
// thread. Destroying the device from the Node thread while such a timer is
// queued or mid-callback is a use-after-free — tester crash 2026-07-15:
// ASIOAudioIODevice::timerCallback → reloadChannelNames on a freed device,
// ~5 min after start, every run (Focusrite USB ASIO reset request).
// Hopping the mutation onto the message thread serialises it with those
// timer callbacks, so a timer can never observe a half-destroyed device.
//
// Windows-only hop, by design: macOS's dispatchOnMessageThread already runs
// inline (no separate pump), and Linux/ALSA keeps its long-standing
// "called from the Node main thread" contract untouched. Inline when the
// caller already IS the message thread (engine init runs there), because
// dispatch-and-wait from the message thread would deadlock.
//
// P0 (guide §12): routed through the LifecycleExecutor. False now means the
// op verifiably did NOT run (pump gone, or the engine generation moved while
// it was queued) — the old "false but it may still run later" desync state
// is gone. The wait is unbounded; a watchdog logs every 15 s while a driver
// call blocks the message thread.
// CAPTURE RULE unchanged: the closure must own everything it touches —
// capture by value (engine snapshot, args) and write results through a
// shared_ptr, never through references to the caller's stack.
template <typename Func>
inline bool runDeviceLifecycleOp(Func&& func)
{
#if JUCE_WINDOWS
    return runLifecycleOpOk("device-lifecycle",
                            std::function<void()>(std::forward<Func>(func)));
#else
    // macOS: dispatch runs inline (no separate pump). Linux/ALSA keeps its
    // long-standing "called from the Node main thread" contract untouched.
    func();
    return true;
#endif
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
