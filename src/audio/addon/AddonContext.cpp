// AddonContext implementation — moved verbatim from NodeAddon.cpp (TLC plan
// phase 6 / §3.1). See AddonContext.h for the lifetime rules.

#include "AddonContext.h"

#include "../Sandbox/CrashAttribution.h"

#include <chrono>
#include <cstdio>
#include <set>
#include <thread>

namespace slopsmith::addon {

static std::shared_ptr<AudioEngine> engine;
static std::mutex engineMutex;
static std::shared_ptr<VSTHost> vstHost;
static std::mutex vstHostMutex;

static std::thread juceMessageThread;
static std::atomic<bool> juceRunning{false};
static std::atomic<bool> alreadyShutDown{false};

// Runs on the message thread at the start of shutdown, before engine.reset()
// frees the processors any editor windows point at (#56). Set by initialize().
static std::function<void()> shutdownUiTeardown;

std::shared_ptr<AudioEngine> snapshotEngine()
{
    std::lock_guard<std::mutex> lock(engineMutex);
    return engine;
}

std::shared_ptr<VSTHost> snapshotVstHost()
{
    std::lock_guard<std::mutex> lock(vstHostMutex);
    return vstHost;
}

// ── JUCE Message Thread ──────────────────────────────────────────────────────
// JUCE requires a message thread for plugin loading, audio device management,
// etc. We pump it in a dedicated thread.

static void startJuceMessageThread()
{
    if (juceRunning.load()) return;
    juceRunning.store(true);

#if JUCE_MAC
    // On macOS, JUCE's MessageManager::runDispatchLoopUntil internally calls
    // `-[NSApplication _nextEventMatchingEventMask:...]`, which AppKit asserts
    // must run on the true main thread. Node.js already owns the main thread
    // (running libuv's event loop), so we can't spawn a second NS event pump
    // without hitting `nextEventMatchingMask should only be called from the
    // Main Thread!` and aborting.
    //
    // Workaround: designate Node's current thread as JUCE's message thread and
    // skip the dispatch loop. callAsync()'d callbacks will still queue; we
    // drain them from the Node thread via a libuv timer created below.
    juce::MessageManager::getInstance();
#else
    juceMessageThread = std::thread([]() {
        juce::MessageManager::getInstance();
        while (juceRunning.load())
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        }
        juce::MessageManager::deleteInstance();
    });
#endif
}

static void stopJuceMessageThread()
{
    juceRunning.store(false);
#if !JUCE_MAC
    if (juceMessageThread.joinable())
        juceMessageThread.join();
#else
    juce::MessageManager::deleteInstance();
#endif
}

bool dispatchOnMessageThreadImpl(std::function<void()> func)
{
#if JUCE_MAC
    // No background message thread on macOS — execute inline on caller thread.
    // Audio device / NAM / IR init is thread-safe for our use; VST/AU plugin
    // instantiation (which genuinely requires a message thread on macOS) is
    // the one capability we give up until a proper libuv-based pump lands.
    func();
    return true;
#else
    // Heap-allocate the WaitableEvent and capture by value so the queued
    // callAsync closure can outlive this stack frame. Without this, a 15 s
    // timeout (rare, but possible during shutdown when the message thread is
    // busy) leaves the lambda running on freed `done` storage — a real UAF.
    //
    // Both failure modes are reported to the caller: a refused post means
    // `func` will NEVER run (message queue already gone); a wait timeout
    // means it hasn't run YET (it may still run later while the dispatch
    // loop drains). Lifecycle callers must not proceed as if the work
    // completed — doShutdown in particular used to unload the addon while
    // editor teardown / stopAudio / engine destruction were still pending.
    auto done = std::make_shared<juce::WaitableEvent>();
    const bool posted = juce::MessageManager::callAsync(
        [func = std::move(func), done]() mutable {
            func();
            done->signal();
        });
    if (!posted)
    {
        fprintf(stderr, "[audio-native] dispatchOnMessageThread: message queue "
                        "refused the post; dispatched work will not run\n");
        return false;
    }
    if (!done->wait(15000))
    {
        fprintf(stderr, "[audio-native] dispatchOnMessageThread: dispatched work "
                        "did not complete within 15s\n");
        return false;
    }
    return true;
#endif
}

// ── Pending async loads ──────────────────────────────────────────────────────

static std::mutex pendingLoadsMutex;
static std::set<std::shared_ptr<juce::WaitableEvent>> pendingLoads;

bool isShuttingDown()
{
    return alreadyShutDown.load(std::memory_order_acquire);
}

void registerPendingLoad(std::shared_ptr<juce::WaitableEvent> evt)
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    pendingLoads.insert(std::move(evt));
}

void unregisterPendingLoad(const std::shared_ptr<juce::WaitableEvent>& evt)
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    pendingLoads.erase(evt);
}

void cancelAllPendingLoads()
{
    std::lock_guard<std::mutex> lock(pendingLoadsMutex);
    for (auto& evt : pendingLoads) evt->signal();
    pendingLoads.clear();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void initialize(std::function<void()> uiTeardownHook)
{
    shutdownUiTeardown = std::move(uiTeardownHook);

    // Reset the shutdown latch so a JS-level init→shutdown→init cycle (e.g.
    // a test harness recreating the engine) actually runs shutdown again
    // instead of treating it as already-done.
    alreadyShutDown.store(false, std::memory_order_release);

    // Start JUCE message thread first (no-op on macOS)
    startJuceMessageThread();

#if !JUCE_MAC
    // Small delay to ensure message thread is pumping
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

    // Create engine on the JUCE message thread (or inline on macOS)
    const bool initialized = dispatchOnMessageThread([]() {
        std::shared_ptr<AudioEngine> liveEngine;
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            engine = std::make_shared<AudioEngine>();
            liveEngine = engine;
        }
        {
            std::lock_guard<std::mutex> lock(vstHostMutex);
            vstHost = std::make_shared<VSTHost>();
        }

        auto types = liveEngine->getDeviceTypes();
        fprintf(stderr, "[audio-native] Init complete. Device types: %d\n", types.size());
        for (int i = 0; i < types.size(); ++i)
            fprintf(stderr, "[audio-native]   %s: %d inputs, %d outputs\n",
                    types[i].name.toRawUTF8(),
                    types[i].inputDevices.size(),
                    types[i].outputDevices.size());
    });
    if (!initialized)
        fprintf(stderr, "[audio-native] initialize: engine creation did not complete "
                        "on the message thread; audio bindings will no-op until re-init\n");
}

void doShutdown()
{
    // The latch is flipped at the TOP rather than the bottom so a
    // re-entrant call (e.g. env-cleanup-hook firing while a JS-level
    // shutdown is mid-flight) bails immediately rather than racing on
    // the same teardown sequence. Assumed serialisation invariants:
    //   - dispatchOnMessageThread is single-writer to engine/vstHost
    //     (both touched only here or from initialize);
    //   - stopJuceMessageThread is idempotent and safe to call when the
    //     thread was never started (defensive checks inside).
    // If a future caller mutates engine/vstHost between this latch and
    // the dispatch (or the dispatch's 15s wait times out), THIS call's
    // body may not finish before returning — but the re-entrant
    // cleanup-hook will then no-op via the latch and the dispatch
    // queue itself unwinds whatever's pending. Net result: at-most-
    // once execution of the gated body, even under teardown races.
    bool expected = false;
    if (!alreadyShutDown.compare_exchange_strong(expected, true)) return;

    // Release any LoadVSTWorker / LoadPresetWorker currently blocked on a
    // pending async load. Without this they'd wait forever on the
    // WaitableEvent — the createPluginInstanceAsync callback can't fire
    // once the message thread is gone.
    cancelAllPendingLoads();

    if (juceRunning.load() || snapshotEngine() || snapshotVstHost())
    {
        const bool toreDown = dispatchOnMessageThread([]() {
            // Editors reference their slot's processor; engine.reset() below
            // frees the whole chain, so destroy the editor windows first (#56).
            if (shutdownUiTeardown) shutdownUiTeardown();
            if (auto liveEngine = snapshotEngine())
                liveEngine->stopAudio();
            {
                std::lock_guard<std::mutex> lock(engineMutex);
                engine.reset();
            }
            {
                std::lock_guard<std::mutex> lock(vstHostMutex);
                vstHost.reset();
            }
        });
        if (!toreDown)
        {
            // Editor teardown / stopAudio / engine destruction have NOT
            // completed. Do not stop the message thread underneath them: a
            // timed-out teardown lambda is still queued and can only finish
            // if the pump keeps running. Leaking the pump thread at process
            // exit beats unloading the addon mid-destruction (the exact
            // shutdown UAF this path exists to prevent). The latch stays
            // set, so a re-entrant shutdown call no-ops.
            fprintf(stderr, "[audio-native] doShutdown: engine teardown did not "
                            "complete; leaving message thread running\n");
            slopsmith::sandbox::uninstallVstCrashAttribution();
            return;
        }
    }

    stopJuceMessageThread();

    // Restore the previous top-level exception filter — the addon (and thus our
    // unhandledFilter's code) may be unloaded, so it must not stay installed.
    slopsmith::sandbox::uninstallVstCrashAttribution();
}

} // namespace slopsmith::addon
