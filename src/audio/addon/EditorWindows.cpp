// EditorWindows implementation — moved verbatim from NodeAddon.cpp (TLC plan
// phase 7 / §3.4). Only edits: statics live in this namespace now, and the
// two bindings validate their slot-id argument through NapiHelpers (the same
// deep-read 2 fix the other bindings got in phase 6 — a NaN slot id used to
// coerce to slot 0 and open/close the wrong editor).

#include "EditorWindows.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "ChainOps.h"
#include "../Sandbox/SandboxedProcessor.h"
#include "../Sandbox/CrashAttribution.h"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace slopsmith::addon {

class PluginEditorWindow;
static std::map<int, std::unique_ptr<PluginEditorWindow>> editorWindows;

class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow(juce::AudioProcessorEditor* ed, const juce::String& title)
        : DocumentWindow(title, juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setContentOwned(ed, true);
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        centreWithSize(ed->getWidth(), ed->getHeight());
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        // Remove from map so editor can be reopened
        for (auto it = editorWindows.begin(); it != editorWindows.end(); ++it)
        {
            if (it->second.get() == this)
            {
                auto slotId = it->first;
                juce::MessageManager::callAsync([slotId]() {
                    editorWindows.erase(slotId);
                });
                break;
            }
        }
        setVisible(false);
    }
};

// Inline teardown: destroys every editor window. Caller MUST already be on the
// message thread (editorWindows holds JUCE GUI objects). Forward-declared near
// Init for doShutdown's use.
void destroyAllPluginEditorWindowsOnMessageThread()
{
    // Fails fast in assertion-enabled builds if a caller violates the
    // precondition. Compiled out here under -DJUCE_DISABLE_ASSERTIONS, so it is
    // documentation + a debug-build tripwire, never runtime cost.
    JUCE_ASSERT_MESSAGE_THREAD
    editorWindows.clear();
}

// See the forward declaration above ClearChain for why this exists. Tears down
// the in-process editor windows so they are destroyed before the caller frees
// the processors those editors point at. Clearing an empty map is cheap, so
// calling this on every teardown is fine even when no editor is open.
//
// IMPORTANT: every caller runs on a MAIN-thread / message-thread context —
// ClearChain and LoadPreset are N-API calls on the Node thread, doShutdown uses
// the inline variant directly. This is NOT called from a libuv worker (that is
// why LoadPreset closes editors before queuing LoadPresetWorker, rather than
// letting the worker do it). Given that:
//   - Already on the message thread (doShutdown; ClearChain / LoadPreset on
//     macOS, where Node's main thread IS the JUCE message thread) → tear down
//     inline; posting-and-waiting on ourselves would deadlock.
//   - Otherwise (ClearChain / LoadPreset on Linux/Windows, where the JUCE
//     message thread is a dedicated std::thread) → post to that thread and block
//     until the editors are gone. Its 50ms dispatch loop drains this promptly,
//     so there is no macOS-style stall here. Report a refused post / wait
//     timeout so a lingering-editor UAF stays diagnosable.
bool closeAllPluginEditorWindows()
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && mm->isThisTheMessageThread())
    {
        destroyAllPluginEditorWindowsOnMessageThread();
        return true;
    }

    auto done = std::make_shared<juce::WaitableEvent>();
    const bool posted = juce::MessageManager::callAsync([done]()
    {
        destroyAllPluginEditorWindowsOnMessageThread();
        done->signal();
    });
    if (!posted)
    {
        fprintf(stderr, "[audio-native] closeAllPluginEditorWindows: message queue refused the post; "
                        "editors may still be alive\n");
        return false;
    }
    if (!done->wait(15000))
    {
        // The queued teardown hasn't run: editors may still hold pointers into
        // the chain. Callers must NOT free slot processors on a false return —
        // proceeding here is exactly the #56 use-after-free, just delayed.
        fprintf(stderr, "[audio-native] closeAllPluginEditorWindows: editor teardown did not complete "
                        "within 15s; caller must not free chain processors\n");
        return false;
    }
    return true;
}

Napi::Value OpenPluginEditor(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    const auto slotIdOpt = argSlotId(info, 0);
    if (!liveEngine || !slotIdOpt)
        return Napi::Boolean::New(env, false);
    const int slotId = *slotIdOpt;

    // Rebuild barrier (ChainOps.h): a chain clear/rebuild is between its
    // editor teardown and the mutation itself — the processor this editor
    // would bind to is about to be freed (#56). Refuse to open.
    if (slopsmith::addon::isChainRebuildPending())
        return Napi::Boolean::New(env, false);

    // Resolve the slot under the chain-mutation mutex: getSlot returns a raw
    // pointer a concurrent worker's clear()/rebuild would free under us.
    // try_lock, never a blocking lock — a preset load can hold the mutex for
    // seconds (VST init) and this is V8's thread; if a mutation is in flight
    // the slot we'd open is about to be replaced anyway.
    std::unique_lock<std::mutex> chainLock(
        slopsmith::addon::chainMutationMutex(), std::try_to_lock);
    if (!chainLock.owns_lock())
        return Napi::Boolean::New(env, false);

    auto slot = liveEngine->getSignalChain().getSlot(slotId);
    if (!slot || !slot->processor || !slot->processor->hasEditor())
        return Napi::Boolean::New(env, false);

    // Sandboxed plugins: the editor is a top-level window owned by the
    // sandbox child process. No host-side PluginEditorWindow and no
    // cross-process SetParent reparent — that path produced a blank
    // rendered surface for D3D / OpenGL plugins (Neural DSP Archetypes,
    // etc.) because their render context lives in the child. The child's
    // kOpenEditor handler brings the existing window to front on a repeat
    // click, so re-entry is cheap and we don't track host-side state.
    //
    // Dispatch off the N-API call thread: requestOpenEditor() uses a
    // blocking control->request (kDefaultReplyTimeoutMs = 10s), which on
    // a slow or hung sandbox would otherwise stall V8's JS thread for
    // the full timeout. Capture slotId rather than a raw processor
    // pointer and re-resolve inside the message-thread lambda — that
    // closes a UAF window where the slot could be removed (or the engine
    // torn down) between this call returning and the async firing.
    // Return optimistically; matches the in-process path below.
    //
    // SandboxedProcessor is compiled on all desktop platforms now (the POSIX
    // sandbox runtime is active — see src/audio/CMakeLists.txt), so the
    // editor-open IPC routes to the sandbox child on macOS/Linux too. The
    // child owns a floating editor window (Reaper-style); the host only tracks
    // the open/closed bit.
#if defined(SLOPSMITH_AUDIO_ADDON)
    if (auto* sb = dynamic_cast<slopsmith::sandbox::SandboxedProcessor*>(slot->processor.get()))
    {
        // Synchronous gate: if the sandbox child is already gone (crashed
        // or shut down) there's no point scheduling the IPC. Return false
        // so the renderer can surface "editor unavailable" rather than
        // toggling its UI into a fake-open state that no event will ever
        // contradict. hasEditor() above already gated on isAlive() but a
        // crash between then and now is possible — re-check here.
        if (!sb->isAlive())
            return Napi::Boolean::New(env, false);
        // Validation is done — release before queueing, so the lambda's own
        // try_lock on the message thread can't collide with THIS thread still
        // holding the mutex and drop the open as a false conflict.
        chainLock.unlock();
        const bool queued = juce::MessageManager::callAsync([slotId]()
        {
            auto liveEngine = snapshotEngine();
            if (!liveEngine) return;
            // try_lock, NEVER a blocking lock on the message thread: chain
            // workers holding the mutex block-wait on this very thread
            // (loadVstSandboxAware's callAsync+wait) — blocking here would
            // deadlock. Contention means a mutation is rebuilding the slot;
            // skip the open.
            std::unique_lock<std::mutex> chainLock(
                slopsmith::addon::chainMutationMutex(), std::try_to_lock);
            if (!chainLock.owns_lock()) return;
            if (auto* slot = liveEngine->getSignalChain().getSlot(slotId))
                if (auto* sb = dynamic_cast<slopsmith::sandbox::SandboxedProcessor*>(slot->processor.get()))
                    sb->requestOpenEditor();
        });
        if (!queued)
        {
            // Message queue refused the post — typically only during
            // shutdown. Surface the failure so the renderer doesn't
            // toggle its UI into a fake-open state.
            return Napi::Boolean::New(env, false);
        }
        return Napi::Boolean::New(env, true);
    }
#endif

    // In-process plugin — host-side PluginEditorWindow flow. Everything —
    // including the duplicate-window check — runs on the message thread:
    // editorWindows is a plain std::map owned by that thread, and reading or
    // erasing it from this (N-API) thread raced the message-thread inserts/
    // erases.
    //
    // Capture slotId only — re-resolve the slot via snapshotEngine() +
    // getSlot(slotId) inside the lambda so a SignalChain::removeProcessor()
    // between this call returning and the async firing can't leave us calling
    // createEditorAndMakeActive() on a dangling juce::AudioProcessor*.
    //
    // Validation is done — release before queueing, so the lambda's own
    // try_lock on the message thread can't collide with THIS thread still
    // holding the mutex and drop the open as a false conflict.
    chainLock.unlock();
    const bool queued = juce::MessageManager::callAsync([slotId]()
    {
        // If a window already exists for this slot, bring it to front rather
        // than creating a duplicate.
        auto it = editorWindows.find(slotId);
        if (it != editorWindows.end() && it->second)
        {
            if (it->second->isVisible())
            {
                it->second->toFront(true);
                return;
            }
            // Window was hidden/closed, remove stale entry
            editorWindows.erase(it);
        }

        auto liveEngine = snapshotEngine();
        if (!liveEngine) return;
        // try_lock, NEVER a blocking lock on the message thread: chain
        // workers holding the mutex block-wait on this very thread
        // (loadVstSandboxAware's callAsync+wait) — blocking here would
        // deadlock. Contention means the slot is being rebuilt; skip.
        std::unique_lock<std::mutex> chainLock(
            slopsmith::addon::chainMutationMutex(), std::try_to_lock);
        if (!chainLock.owns_lock()) return;
        auto& chain = liveEngine->getSignalChain();
        auto* slot = chain.getSlot(slotId);
        if (!slot || !slot->processor) return;

        // ── Windows editor-crash class fix ───────────────────────────────────
        // An in-process VST3 editor is created on JUCE's BACKGROUND message
        // thread (V8 owns the OS main thread inside a Node addon). On Windows a
        // Qt-using / window-on-init plugin then faults via USER32->WndProc on
        // WM_ACTIVATEAPP with NO host frame on the stack, so the SignalChain SEH
        // guard can't catch it and the whole app dies (0xC0000005 / 0xC0000409).
        // Fix: never open a VST3 editor in-process on Windows — promote the slot
        // to the out-of-process sandbox (which hosts the editor on a real
        // top-level message thread, the environment the plugin needs) and open
        // it there. Compiled on every platform so the swap path keeps building;
        // gated to Windows at runtime since the in-process editor is fine on
        // macOS/Linux (no WndProc) and the sandbox hop is pure overhead there.
        static constexpr bool kPromoteEditorToSandbox =
           #if JUCE_WINDOWS
            true;
           #else
            false;
           #endif
        if (kPromoteEditorToSandbox)
        {
            // Decide + snapshot state SAFELY. captureVstStateForPromotion runs
            // hasEditor()/getStateInformation() under the audio lock and the SEH
            // guard (see its contract), so they neither race process()'s
            // processBlock nor fault the app — an UNguarded getStateInformation on
            // the very plugins this promotion targets would reintroduce the editor
            // crash on the message thread. It returns true only for a non-sandboxed
            // in-process VST3 that actually has an editor.
            juce::MemoryBlock state;
            if (chain.captureVstStateForPromotion(slotId, state))
            {
                const juce::String path = slot->path;   // immutable; message-thread only
                fprintf(stderr, "[AudioEngine] editor-open: promoting in-process VST3 to sandbox: slot %d '%s'\n",
                        slotId, path.toRawUTF8());

                juce::PluginDescription desc;
                desc.fileOrIdentifier = path;
                desc.name = juce::File(path).getFileNameWithoutExtension();

                // tryLoadSandboxed only accepts a plugin that shouldSandbox()
                // approves, so pin this path to the runtime sandbox list first.
                // Remember whether it was ALREADY pinned: if the promotion fails
                // we undo only OUR pin below, so a healthy, never-crashed plugin
                // isn't left permanently forced to a sandbox that just proved
                // unavailable (while a pre-existing/real blocklist entry stays).
                const bool wasAlreadyPinned = slopsmith::sandbox::isCrashedPlugin(path);
                slopsmith::sandbox::addCrashedPlugin(path);

                bool promoted = false;
                juce::String err;
                auto sandboxed = slopsmith::sandbox::tryLoadSandboxed(
                    desc, chain.getCurrentSampleRate(), chain.getCurrentBlockSize(), err);
                if (sandboxed)
                {
                    if (state.getSize() > 0)
                        sandboxed->setStateInformation(state.getData(), (int) state.getSize());
                    if (chain.replaceProcessor(slotId, std::move(sandboxed)))
                    {
                        promoted = true;
                        // A promotion swaps the slot's processor: bump the
                        // generation (we hold chainMutationMutex via the
                        // try_lock above) so JS-side chain owners re-sync
                        // instead of driving the replaced slot blind.
                        slopsmith::addon::bumpChainGeneration();
                        bool editorOpened = false;
                        if (auto* slot2 = chain.getSlot(slotId))
                            if (auto* sb = dynamic_cast<slopsmith::sandbox::SandboxedProcessor*>(slot2->processor.get()))
                                editorOpened = sb->requestOpenEditor();
                        fprintf(stderr, "[AudioEngine] editor-open: sandbox promotion OK for slot %d (editor %s)\n",
                                slotId, editorOpened ? "opened" : "FAILED to open");
                    }
                    else
                    {
                        fprintf(stderr, "[AudioEngine] editor-open: replaceProcessor failed for slot %d\n", slotId);
                    }
                }
                else
                {
                    fprintf(stderr, "[AudioEngine] editor-open: sandbox promotion failed for '%s': %s\n",
                            path.toRawUTF8(), err.toRawUTF8());
                }

                // Undo our transient pin on failure so a plugin that never crashed
                // isn't stranded on the (evidently unavailable) sandbox route.
                if (! promoted && ! wasAlreadyPinned)
                    slopsmith::sandbox::removeCrashedPlugin(path);

                // Promoted or not, never fall through to the in-process editor on
                // Windows — that is the WndProc/Qt crash path this branch exists
                // to avoid.
                return;
            }
            // Not promotable (non-VST / editor-less / already-sandboxed, or the
            // guarded capture faulted and released the processor). Fall through to
            // the in-process branch below, which is safe for all of those cases
            // (an already-sandboxed slot opens its editor out-of-process; an
            // editor-less or released processor simply opens no window).
        }

        // In-process editor: non-VST3, editor-less, already-sandboxed, or POSIX
        // (where the in-process editor is safe).
        //
        // Re-check the processor: the promotion branch above documents that a
        // faulted captureVstStateForPromotion() can RELEASE the slot's
        // processor before returning false — falling through here with a null
        // processor would crash on createEditorAndMakeActive().
        auto* processor = slot->processor.get();
        if (processor == nullptr)
            return;
        auto name = slot->name;
        juce::AudioProcessorEditor* editor = nullptr;
        try {
            editor = processor->createEditorAndMakeActive();
        } catch (const std::exception& e) {
            fprintf(stderr, "[AudioEngine] createEditorAndMakeActive crashed for '%s': %s\n", name.toRawUTF8(), e.what());
        } catch (...) {
            fprintf(stderr, "[AudioEngine] createEditorAndMakeActive crashed for '%s': unknown error\n", name.toRawUTF8());
        }
        if (editor)
        {
            editorWindows[slotId] = std::make_unique<PluginEditorWindow>(editor, name);
            fprintf(stderr, "[AudioEngine] Opened editor for slot %d: %s (%dx%d)\n",
                    slotId, name.toRawUTF8(), editor->getWidth(), editor->getHeight());
        }
    });

    return Napi::Boolean::New(env, queued);
}

Napi::Value ClosePluginEditor(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    const auto slotIdOpt = argSlotId(info, 0);
    if (!slotIdOpt) return Napi::Boolean::New(env, false);
    const int slotId = *slotIdOpt;

    // One queued lambda handles both the sandbox and in-process paths, for
    // two reasons:
    //   - editorWindows is message-thread-owned; the old synchronous
    //     find() here raced the message-thread inserts/erases.
    //   - getSlot() from this (N-API) thread dereferenced a slot a chain
    //     worker could free mid-call; the slot is now resolved inside the
    //     lambda under a try_lock on the chain-mutation mutex.
    // requestCloseEditor() ultimately writes to the control pipe (writeFrame
    // can block up to ~5s on a stalled reader), so dispatching also keeps a
    // slow sandbox from freezing JS / the renderer UI.
    const bool queued = juce::MessageManager::callAsync([slotId]()
    {
        // Host-side window (in-process plugins). Erasing a missing key is a
        // no-op; sandbox slots never have an entry here.
        editorWindows.erase(slotId);

#if defined(SLOPSMITH_AUDIO_ADDON)
        auto liveEngine = snapshotEngine();
        if (!liveEngine) return;
        // try_lock, NEVER a blocking lock on the message thread: chain
        // workers holding the mutex block-wait on this very thread —
        // blocking here would deadlock. Contention means the chain is being
        // rebuilt, which tears editors down anyway.
        std::unique_lock<std::mutex> chainLock(
            slopsmith::addon::chainMutationMutex(), std::try_to_lock);
        if (!chainLock.owns_lock()) return;
        if (auto* slot = liveEngine->getSignalChain().getSlot(slotId))
            if (auto* sb = dynamic_cast<slopsmith::sandbox::SandboxedProcessor*>(slot->processor.get()))
                sb->requestCloseEditor();
#endif
    });
    return Napi::Boolean::New(env, queued);
}


} // namespace slopsmith::addon
