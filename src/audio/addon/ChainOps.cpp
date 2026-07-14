// ChainOps implementation — the chain-mutating async workers, their N-API
// handlers, and loadVstSandboxAware, moved verbatim from NodeAddon.cpp (TLC
// plan phase 7b / 3.3). The serialization primitives (chainMutationMutex /
// chainGeneration) landed in phase 7a; every worker Execute() below holds
// the mutex for its full body.

#include "ChainOps.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "EditorWindows.h"
#include "../AudioEngine.h"
#include "../VSTHost.h"
#include "../VSTTrace.h"
#include "../NAMProcessor.h"
#include "../IRLoader.h"
#include "../Sandbox/SandboxedProcessor.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace slopsmith::addon {

std::mutex& chainMutationMutex()
{
    static std::mutex m;
    return m;
}

static std::atomic<uint64_t> chainGeneration{0};

uint64_t bumpChainGeneration()
{
    return chainGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

uint64_t currentChainGeneration()
{
    return chainGeneration.load(std::memory_order_acquire);
}

// ── Rebuild barrier (see ChainOps.h) ────────────────────────────────────────

static std::atomic<int> chainRebuildsPending{0};

void beginChainRebuild()
{
    chainRebuildsPending.fetch_add(1, std::memory_order_acq_rel);
}

void endChainRebuild()
{
    chainRebuildsPending.fetch_sub(1, std::memory_order_acq_rel);
}

bool isChainRebuildPending()
{
    return chainRebuildsPending.load(std::memory_order_acquire) > 0;
}

// ── decodeStateBlob (moved verbatim) ────────────────────

// Decode a state blob that may be in EITHER base64 flavour. JUCE's
// MemoryBlock::fromBase64Encoding only understands JUCE's own proprietary
// format ("<size>.<juce-alphabet>") and returns false for standard RFC-4648
// base64 — which is what the Python-side plugins (rig_builder et al.) emit
// for per-slot state. That silent false meant setState() was never called
// for those slots: IR stages lost their per-stage `gain` (the cab loudness
// makeup and amp trims never reached the engine). Try the JUCE format first
// (engine-native saves), then fall back to standard b64.
//
// `allowStandard` is only set for IR/NAM slots: their processors take a JSON
// state ({"irPath","gain"} / model path), which is exactly what the plugins
// emit. VST slots keep the JUCE-only decode — their plugin-emitted blobs are
// metadata wrappers, not real setStateInformation() chunks, and feeding those
// to a VST3 for the first time would be an unasked-for behaviour change.
bool decodeStateBlob(const juce::String& s, juce::MemoryBlock& mb,
                            bool allowStandard)
{
    if (mb.fromBase64Encoding(s) && mb.getSize() > 0)
        return true;
    if (!allowStandard)
        return false;
    mb.reset();
    juce::MemoryOutputStream mo(mb, false);
    return juce::Base64::convertFromBase64(mo, s) && mb.getSize() > 0;
}

double loadSafeSampleRate(const AudioEngine& eng)
{
    const double sr = eng.getCurrentSampleRate();
    return (std::isfinite(sr) && sr > 0.0) ? sr : 48000.0;
}

int loadSafeBlockSize(const AudioEngine& eng)
{
    const int bs = eng.getCurrentBlockSize();
    return bs > 0 ? bs : 256;
}

// ── loadVstSandboxAware (moved verbatim from NodeAddon.cpp) ────────────────────

// Load a VST3, routing it through the out-of-process sandbox when
// shouldSandbox() says so (the filename pre-seed or the runtime crash
// blocklist), otherwise loading it in-process. The in-process load uses
// VSTHost::loadPluginAsync so the JUCE message thread keeps pumping during
// the plugin's init — critical for plugins like AmpliTube that post WM_USER
// / WM_TIMER messages to themselves while initialising. The sync
// createPluginInstance would block the pump, those self-messages would
// queue forever, and the plugin would end up half-wired (a pointer that
// only gets written by a queued message stays null, and the editor crashes
// on its first WindowProc dispatch — the AmpliTube failure signature).
//
// Threading: on !JUCE_MAC must be called from a libuv worker thread (NOT
// the JS main thread, NOT the JUCE message thread) — the done->wait below
// has to be on a thread that *isn't* the one running JUCE's pump or the
// load can't complete. On JUCE_MAC the inline sync fallback is used and
// the caller can be the Node/main thread (which is also JUCE's message
// thread there); LoadVST does exactly that, while LoadPresetWorker still
// hits this from a worker (a pre-existing macOS limitation).
//
// On a *required*-sandbox failure (the plugin matched shouldSandbox but the
// sandbox couldn't spawn) this returns nullptr with `error` set and
// `sandboxRequired` true, so the caller can choose how to surface it —
// LoadVSTWorker throws to JS, LoadPresetWorker just skips the slot.
std::unique_ptr<juce::AudioProcessor> loadVstSandboxAware(
    const juce::String& pluginPath, double sr, int bs,
    juce::String& error, bool& sandboxRequired)
{
    sandboxRequired = false;

    // A plugin persisted in a signal-chain preset can be uninstalled or
    // deleted between runs. Instantiating a VST3 whose module is gone from
    // disk faults deep inside the format loader (a stack-buffer-overrun /
    // 0xC0000409 on Windows) and takes the whole app down on startup — before
    // the crash blocklist or sandbox can ever intervene, because the preset is
    // restored independently of those guards. A native access violation also
    // can't be caught by the renderer's JS try/catch around loadPreset. So
    // pre-flight a cheap existence check here, the single choke point shared by
    // every load path (direct LoadVST and preset restore, in-process and
    // sandboxed, all platforms), and fail soft when the file is missing.
    //
    // Only filesystem paths are judged: VST3/LV2 fileOrIdentifiers are absolute
    // paths (File::exists covers both a .vst3 file and a bundle directory),
    // whereas macOS AudioUnit identifiers ("AudioUnit:...") are not absolute
    // paths and must not be rejected here.
    if (juce::File::isAbsolutePath(pluginPath) && ! juce::File(pluginPath).exists())
    {
        error = "Plugin file not found: " + pluginPath;
        VST_TRACE("loadVstSandboxAware: missing plugin file '%s' — skipping load",
                  pluginPath.toRawUTF8());
        return nullptr;
    }

    juce::PluginDescription probeDesc;
    probeDesc.fileOrIdentifier = pluginPath;
    probeDesc.name = juce::File(pluginPath).getFileNameWithoutExtension();

    if (slopsmith::sandbox::shouldSandbox(probeDesc))
    {
        sandboxRequired = true;
        juce::String sandboxErr;
        auto processor = slopsmith::sandbox::tryLoadSandboxed(
            probeDesc, sr, bs, sandboxErr);
        if (!processor)
        {
            error = "sandbox load failed: "
                  + (sandboxErr.isEmpty() ? juce::String("unknown error")
                                          : sandboxErr);
            VST_TRACE("loadVstSandboxAware: sandbox path declined/failed: %s",
                      sandboxErr.toRawUTF8());
        }
        return processor;
    }

   #if JUCE_MAC
    // macOS has no separate JUCE message thread (see startJuceMessageThread /
    // dispatchOnMessageThread): the JUCE MessageManager is bound to the
    // Node/main thread, and dispatchOnMessageThread historically ran inline
    // on the caller. A callAsync + done->wait pattern would queue a callback
    // to a pump that may never run in this calling context.
    //
    // Fall back to the sync loadPlugin, executed on whichever thread called
    // in — the Node/main thread for LoadVST's JUCE_MAC branch (correct: that
    // *is* the MessageManager thread on macOS), or a libuv worker thread for
    // LoadPresetWorker (the pre-existing macOS constraint). Caveat: the
    // existing dispatchOnMessageThread block on macOS already documents
    // that "VST/AU plugin instantiation (which genuinely requires a message
    // thread on macOS) is the one capability we give up until a proper
    // libuv-based pump lands." LoadPresetWorker has called loadVstSandbox-
    // Aware on a worker thread for ages under exactly the same constraint;
    // moving LoadVST to AsyncWorker brings direct loads under the same
    // (pre-existing) limitation. The AmpliTube-class self-message problem
    // this PR targets is Windows-specific (Electron owns the OS main
    // thread, forcing JUCE's MessageManager onto a background thread that
    // createPluginInstance then blocks); macOS doesn't have that mismatch.
    auto host = snapshotVstHost();
    if (! host) { error = "vstHost not initialised"; return nullptr; }
    juce::String err;
    auto instance = host->loadPlugin(pluginPath, sr, bs, err);
    if (! instance) error = err.isNotEmpty() ? err : juce::String("load failed");
    return instance;
   #else
    // In-process: kick off createPluginInstanceAsync on the message thread,
    // block *this* (libuv worker) thread on a WaitableEvent until the load
    // callback fires. The message thread keeps pumping during the wait so
    // the plugin's self-posted init messages dispatch and its state finishes
    // wiring up before the editor is ever opened.
    //
    // All state passed across the thread hop is held by shared_ptr so it
    // outlives the lambda even on an unexpected destructor / scope exit.
    auto instance  = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>>();
    auto loadError = std::make_shared<juce::String>();
    auto done      = std::make_shared<juce::WaitableEvent>();

    // Register BEFORE scheduling so a shutdown that lands between callAsync
    // and the wait below can't miss us — cancelAllPendingLoads would
    // otherwise see an empty set and the worker would block forever.
    registerPendingLoad(done);

    // Check alreadyShutDown after registering to catch the inverse race
    // (shutdown ran before we registered): if it's already set, the
    // shutdown won't see this event and we must bail ourselves.
    if (slopsmith::addon::isShuttingDown())
    {
        unregisterPendingLoad(done);
        error = "shutdown in flight";
        return nullptr;
    }

    // Snapshot a shared_ptr to vstHost so the async load and its inner
    // continuation can keep VSTHost (and thus formatManager) alive even if
    // shutdown resets the global mid-load. The inner callback captures the
    // same hostKeeper, so JUCE retains it until createPluginInstanceAsync
    // completes; once the callback destructs, the keeper drops, and if the
    // global has been reset by then the VSTHost destructor runs safely
    // (no work in flight). The snapshot itself goes through vstHostMutex
    // so the shared_ptr copy can't race with shutdown's vstHost.reset().
    auto hostKeeper = snapshotVstHost();

    const bool scheduled = juce::MessageManager::callAsync(
        [hostKeeper, pluginPath, sr, bs, instance, loadError, done]()
        {
            // Shutdown may have fired between callAsync queueing this
            // lambda and the message thread picking it up. Bail before
            // kicking off another in-flight createPluginInstanceAsync
            // that the shutdown would otherwise have to wait on.
            if (slopsmith::addon::isShuttingDown())
            {
                *loadError = "shutdown in flight";
                done->signal();
                return;
            }
            if (! hostKeeper)
            {
                *loadError = "vstHost not initialised";
                done->signal();
                return;
            }
            hostKeeper->loadPluginAsync(
                pluginPath, sr, bs,
                [hostKeeper, instance, loadError, done]
                (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err)
                {
                    *instance  = std::move(inst);
                    *loadError = std::move(err);
                    done->signal();
                });
        });

    if (! scheduled)
    {
        // The message queue is gone (typically: shutdown in flight). The
        // lambda will never run, so done would never signal — surface the
        // failure rather than hanging the worker forever.
        unregisterPendingLoad(done);
        error = "message manager unavailable (shutdown?)";
        return nullptr;
    }

    // No timeout: createPluginInstanceAsync is genuinely async (the message
    // thread keeps pumping), so a slow first-run plugin (e.g. one doing a
    // license check that exceeds 15 s) is allowed to take however long it
    // takes. The old 15-second timeout in dispatchOnMessageThread could
    // return early while the lambda was still running, then the lambda
    // would construct a fully-initialised plugin only for it to immediately
    // destruct because no one held a reference — running VST teardown on
    // the message thread while the user had already moved on. That race is
    // gone with this design.
    //
    // Tradeoff: this call holds a libuv threadpool worker for the duration
    // of the plugin's init. Multiple concurrent hung loads could in theory
    // starve other AsyncWorkers (fs / crypto). In practice plugin loads are
    // user-driven and serialised (LoadPresetWorker loads slots one at a
    // time), and a truly stuck load is bounded by app shutdown via
    // cancelAllPendingLoads. A proper "fire-and-forget with a TSFN
    // completion callback" model would eliminate the block entirely but
    // requires a bigger API restructure than this PR's scope.
    done->wait();
    unregisterPendingLoad(done);

    // Distinguish "shutdown cancelled us before the callback fired"
    // (instance null AND error empty) from a normal load failure (instance
    // null with error set) and a normal success.
    if (! *instance && loadError->isEmpty())
    {
        error = "load cancelled (shutdown)";
        return nullptr;
    }
    error = *loadError;
    return std::move(*instance);
   #endif
}

// AsyncWorker wrapper for LoadVST. Execute() runs on a libuv worker thread,
// so loadVstSandboxAware can block-wait on the async load without freezing
// the JS main thread or deadlocking the JUCE message thread.

// ── VST/NAM/IR workers + handlers (moved verbatim from NodeAddon.cpp) ────────────────────

class LoadVSTWorker : public Napi::AsyncWorker
{
public:
    LoadVSTWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env)
        , deferred_(deferred)
        , pluginPath_(std::move(path)) {}

    void Execute() override
    {
        // Serialize the FULL mutation (TLC deep-read 1): overlapping chain
        // workers on the libuv pool must not interleave clear()/addProcessor().
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        // Snapshot engine + vstHost through their mutex-protected helpers so
        // shutdown's reset on the message thread can't race the worker's
        // dereferences below. The shared_ptr locals keep both objects alive
        // for the duration of this worker even if the globals get reset
        // mid-load. The atomic alreadyShutDown gate is the early-out: once
        // it's set, the dispatched reset is on its way and there's no point
        // continuing.
        if (slopsmith::addon::isShuttingDown())
        {
            error_ = "shutdown in flight";
            return;
        }
        auto engineKeeper = snapshotEngine();
        auto hostSnap     = snapshotVstHost();
        if (!engineKeeper || !hostSnap)
        {
            error_ = "engine not initialised";
            return;
        }

        const auto sr = loadSafeSampleRate(*engineKeeper);
        const auto bs = loadSafeBlockSize(*engineKeeper);
        const auto path = juce::String(pluginPath_);
        VST_TRACE("LoadVSTWorker: path='%s' sr=%.0f bs=%d",
                  pluginPath_.c_str(), sr, bs);

        bool sandboxRequired = false;
        juce::String err;
        auto processor = loadVstSandboxAware(path, sr, bs, err, sandboxRequired);

        if (sandboxRequired && !processor)
        {
            // The plugin's on the denylist and the sandbox couldn't spawn —
            // falling back to in-process is what crashed the addon to begin
            // with. Surface as a JS exception (handled in OnOK).
            fprintf(stderr, "[LoadVST] Failed: %s\n", err.toRawUTF8());
            error_ = err;
            sandboxFailed_ = true;
            return;
        }

        if (!processor)
        {
            fprintf(stderr, "[LoadVST] Failed: %s\n", err.toRawUTF8());
            error_ = err;
            return;
        }

        // Engine may have been torn down while we were waiting on the async
        // load. The shared_ptr captures keep `processor` alive; just don't
        // touch a freed engine. The processor destructs cleanly when this
        // scope exits.
        //
        // Gate on alreadyShutDown (atomic, properly synchronised) before the
        // raw engine/vstHost pointer reads — once that flag is set, the
        // dispatched reset of engine/vstHost is on its way and any use of
        // the pointers from this worker thread is racy. The atomic check is
        // the authoritative "should I still be touching engine?" signal.
        if (slopsmith::addon::isShuttingDown())
        {
            error_ = "engine torn down during load";
            return;
        }
        // Re-snapshot the engine — the original engineKeeper might have
        // outlived a reset on the message thread, but the AudioEngine
        // we're about to mutate must be the still-installed one. If the
        // global has been reset, the local keeps the old engine alive but
        // we shouldn't be adding slots to it any more.
        auto liveEngine = snapshotEngine();
        if (!liveEngine || !snapshotVstHost())
        {
            error_ = "engine torn down during load";
            return;
        }

        auto name = processor->getName();
        slotId_ = liveEngine->getSignalChain().addProcessor(
            std::move(processor),
            ProcessorSlot::Type::VST,
            name,
            path);
        if (slotId_ >= 0)
            slopsmith::addon::bumpChainGeneration();  // still under chainLock
    }

    void OnOK() override
    {
        if (sandboxFailed_)
        {
            // Match the prior LoadVST throw-on-required-sandbox-failure
            // behaviour so renderers' try/catch keeps working.
            deferred_.Reject(
                Napi::Error::New(Env(), error_.toStdString()).Value());
            return;
        }
        deferred_.Resolve(Napi::Number::New(Env(), slotId_));
    }

    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string pluginPath_;
    int slotId_ = -1;
    bool sandboxFailed_ = false;
    juce::String error_;
};

Napi::Value LoadVST(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || !snapshotVstHost() || info.Length() < 1)
    {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto pluginPath = info[0].As<Napi::String>().Utf8Value();

   #if JUCE_MAC
    // On macOS the JUCE MessageManager is bound to the Node/main thread.
    // Running this as an AsyncWorker would call vstHost->loadPlugin on a
    // libuv worker thread, which JUCE documents as unsupported for VST/AU
    // instantiation. Do the load synchronously on the Node/main thread
    // (same as the pre-PR LoadVST) and return a resolved Promise to match
    // the new signature. Pays the foreground-block cost the AsyncWorker
    // path was supposed to avoid, but that's the existing macOS reality —
    // dispatchOnMessageThread already runs inline there. The async-load
    // motivation (AmpliTube blocking the background JUCE message thread
    // under Electron) is a Windows-only problem.
    // Snapshot once for the whole load so the same AudioEngine is used for
    // the sr/bs reads and the addProcessor mutation, even if shutdown
    // resets the global mid-call.
    auto liveEngine = snapshotEngine();
    if (! liveEngine)
    {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }
    juce::String error;
    bool sandboxRequired = false;
    auto processor = loadVstSandboxAware(
        juce::String(pluginPath),
        loadSafeSampleRate(*liveEngine),
        loadSafeBlockSize(*liveEngine),
        error, sandboxRequired);

    if (sandboxRequired && !processor)
    {
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());
        deferred.Reject(
            Napi::Error::New(env, error.toStdString()).Value());
        return deferred.Promise();
    }

    int slotId = -1;
    if (processor)
    {
        auto name = processor->getName();
        // Serialize with the async chain workers (deep-read 1): an unguarded
        // addProcessor here could land a slot inside a LoadPresetWorker's
        // clear()+rebuild running on a libuv thread. Deadlock-safe on macOS:
        // a worker holding this mutex never waits on THIS (Node/main) thread —
        // loadVstSandboxAware's JUCE_MAC branch is a synchronous load on the
        // worker itself, and dispatchOnMessageThread runs inline there. Only
        // the mutation is guarded; the slow plugin load above stays outside
        // the lock.
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        slotId = liveEngine->getSignalChain().addProcessor(
            std::move(processor),
            ProcessorSlot::Type::VST,
            name,
            juce::String(pluginPath));
        if (slotId >= 0)
            slopsmith::addon::bumpChainGeneration();  // still under chainLock
    }
    else
    {
        fprintf(stderr, "[LoadVST] Failed: %s\n", error.toRawUTF8());
    }
    deferred.Resolve(Napi::Number::New(env, slotId));
    return deferred.Promise();
   #else
    auto* worker = new LoadVSTWorker(env, deferred, std::move(pluginPath));
    worker->Queue();
    return deferred.Promise();
   #endif
}

class LoadNAMWorker : public Napi::AsyncWorker
{
public:
    LoadNAMWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env), deferred_(deferred), modelPath_(std::move(path)) {}

    void Execute() override
    {
        // Serialize the FULL mutation (TLC deep-read 1): overlapping chain
        // workers on the libuv pool must not interleave clear()/addProcessor().
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { slotId_ = -1; return; }

        auto processor = std::make_unique<NAMProcessor>();
        if (processor->loadModel(juce::File(juce::String(modelPath_))))
        {
            auto name = processor->getModelName();
            slotId_ = liveEngine->getSignalChain().addProcessor(
                std::move(processor),
                ProcessorSlot::Type::NAM,
                "NAM: " + name,
                juce::String(modelPath_));
            if (slotId_ >= 0)
                slopsmith::addon::bumpChainGeneration();  // still under chainLock
        }
    }

    void OnOK() override { deferred_.Resolve(Napi::Number::New(Env(), slotId_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string modelPath_;
    int slotId_ = -1;
};

Napi::Value LoadNAMModel(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || info.Length() < 1) {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto modelPath = info[0].As<Napi::String>().Utf8Value();
    auto worker = new LoadNAMWorker(env, deferred, modelPath);
    worker->Queue();
    return deferred.Promise();
}

class LoadIRWorker : public Napi::AsyncWorker
{
public:
    LoadIRWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string path)
        : Napi::AsyncWorker(env), deferred_(deferred), irPath_(std::move(path)) {}

    void Execute() override
    {
        // Serialize the FULL mutation (TLC deep-read 1): overlapping chain
        // workers on the libuv pool must not interleave clear()/addProcessor().
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { slotId_ = -1; return; }

        const auto sr = loadSafeSampleRate(*liveEngine);
        const auto bs = loadSafeBlockSize(*liveEngine);
        auto processor = std::make_unique<IRLoader>();
        processor->setPlayConfigDetails(2, 2, sr, bs);
        processor->prepareToPlay(sr, bs);
        if (processor->loadIR(juce::File(juce::String(irPath_))))
        {
            auto name = processor->getIRName();
            slotId_ = liveEngine->getSignalChain().addProcessor(
                    std::move(processor),
                    ProcessorSlot::Type::IR,
                    "IR: " + name,
                    juce::String(irPath_));
            if (slotId_ >= 0)
                slopsmith::addon::bumpChainGeneration();  // still under chainLock
        }
    }

    void OnOK() override { deferred_.Resolve(Napi::Number::New(Env(), slotId_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string irPath_;
    int slotId_ = -1;
};

Napi::Value LoadIR(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || info.Length() < 1) {
        deferred.Resolve(Napi::Number::New(env, -1));
        return deferred.Promise();
    }

    auto irPath = info[0].As<Napi::String>().Utf8Value();
    auto worker = new LoadIRWorker(env, deferred, irPath);
    worker->Queue();
    return deferred.Promise();
}

// Replace the IR of an EXISTING convolution slot in place (cab swap / mic move),
// so the rest of the chain — the amp VST above all — is NOT torn down and rebuilt.
// Mirrors LoadIRWorker but calls SignalChain::replaceProcessor(slotId, …) instead
// of addProcessor. Optional `gain` (>=0) updates the slot's post-gain (the cab
// makeup); a negative gain leaves the existing post-gain untouched.
class ReplaceIRWorker : public Napi::AsyncWorker
{
public:
    ReplaceIRWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                    int slotId, std::string path, float gain)
        : Napi::AsyncWorker(env), deferred_(deferred),
          slotId_(slotId), irPath_(std::move(path)), gain_(gain) {}

    void Execute() override
    {
        // Serialize the FULL mutation (TLC deep-read 1): overlapping chain
        // workers on the libuv pool must not interleave clear()/addProcessor().
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { ok_ = false; return; }

        const auto sr = loadSafeSampleRate(*liveEngine);
        const auto bs = loadSafeBlockSize(*liveEngine);
        auto processor = std::make_unique<IRLoader>();
        processor->setPlayConfigDetails(2, 2, sr, bs);
        processor->prepareToPlay(sr, bs);
        if (! processor->loadIR(juce::File(juce::String(irPath_)))) { ok_ = false; return; }

        auto name = processor->getIRName();
        ok_ = liveEngine->getSignalChain().replaceProcessor(
                slotId_, std::move(processor),
                "IR: " + name, juce::String(irPath_));
        if (ok_ && gain_ >= 0.0f)
            liveEngine->getSignalChain().setPostGain(slotId_, gain_);
        if (ok_)
            slopsmith::addon::bumpChainGeneration();  // still under chainLock
    }

    void OnOK() override { deferred_.Resolve(Napi::Boolean::New(Env(), ok_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    int slotId_;
    std::string irPath_;
    float gain_;
    bool ok_ = false;
};

Napi::Value ReplaceIR(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (!snapshotEngine() || info.Length() < 2
        || !info[0].IsNumber() || !info[1].IsString()) {
        deferred.Resolve(Napi::Boolean::New(env, false));
        return deferred.Promise();
    }

    const int slotId = info[0].As<Napi::Number>().Int32Value();
    const auto irPath = info[1].As<Napi::String>().Utf8Value();
    const float gain = (info.Length() >= 3 && info[2].IsNumber())
        ? info[2].As<Napi::Number>().FloatValue() : -1.0f;

    auto worker = new ReplaceIRWorker(env, deferred, slotId, irPath, gain);
    worker->Queue();
    return deferred.Promise();
}


// ── LoadPresetWorker + handler (moved verbatim from NodeAddon.cpp) ────────────────────

class LoadPresetWorker : public Napi::AsyncWorker
{
public:
    LoadPresetWorker(Napi::Env env, Napi::Promise::Deferred deferred, std::string json)
        : Napi::AsyncWorker(env), deferred_(deferred), presetJson_(std::move(json)) {}

    void Execute() override
    {
        // Release the rebuild barrier LoadPreset() armed before editor
        // teardown, on every exit path — editors may open again once the
        // rebuild below has completed (or bailed).
        struct BarrierRelease {
            ~BarrierRelease() { slopsmith::addon::endChainRebuild(); }
        } barrierRelease;

        // Serialize the FULL mutation (TLC deep-read 1): overlapping chain
        // workers on the libuv pool must not interleave clear()/addProcessor().
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        auto liveEngine = snapshotEngine();
        if (!liveEngine) { success_ = false; error_ = "No engine"; return; }

        auto parsed = juce::JSON::parse(juce::String(presetJson_));
        if (!parsed.isObject()) { success_ = false; error_ = "Invalid JSON"; return; }

        auto* root = parsed.getDynamicObject();
        if (!root) { success_ = false; error_ = "Invalid preset"; return; }

        auto chainVar = root->getProperty("chain");
        auto* chainArray = chainVar.getArray();
        if (!chainArray) { success_ = false; error_ = "No chain array"; return; }

        // NB: any open in-process editor windows were already torn down on the
        // message thread by LoadPreset() before this AsyncWorker was queued (see
        // there) — so clearing the chain here can't leave an editor pointing at
        // a freed processor (use-after-free; #56). We deliberately do NOT tear
        // editors down from this worker thread: JUCE GUI objects must only be
        // destroyed on the message thread, and macOS has no pump to marshal to
        // from here.
        // Clear existing chain
        liveEngine->getSignalChain().clear();

        double sr = loadSafeSampleRate(*liveEngine);
        int bs = loadSafeBlockSize(*liveEngine);

        for (auto& slotVar : *chainArray)
        {
            auto* slotObj = slotVar.getDynamicObject();
            if (!slotObj) continue;

            int type = (int)slotObj->getProperty("type");
            auto name = slotObj->getProperty("name").toString();
            auto path = slotObj->getProperty("path").toString();
            bool bypassed = (bool)slotObj->getProperty("bypassed");
            auto stateB64 = slotObj->getProperty("state").toString();

            std::unique_ptr<juce::AudioProcessor> processor;

            if (type == (int)ProcessorSlot::Type::VST && snapshotVstHost())
            {
                // Sandbox-aware load: a crash-blocklisted plugin restored
                // from a preset must still go out-of-process, otherwise the
                // "one crash, then always sandbox" contract is defeated.
                juce::String err;
                bool sandboxRequired = false;
                processor = loadVstSandboxAware(path, sr, bs, err, sandboxRequired);
                if (!processor)
                {
                    fprintf(stderr, "[LoadPreset] VST load failed: %s (%s)\n",
                            name.toRawUTF8(), err.toRawUTF8());
                    continue;
                }
            }
            else if (type == (int)ProcessorSlot::Type::NAM)
            {
                auto nam = std::make_unique<NAMProcessor>();
                if (!nam->loadModel(juce::File(path)))
                {
                    fprintf(stderr, "[LoadPreset] NAM load failed: %s\n", path.toRawUTF8());
                    continue;
                }
                processor = std::move(nam);
            }
            else if (type == (int)ProcessorSlot::Type::IR)
            {
                auto ir = std::make_unique<IRLoader>();
                ir->setPlayConfigDetails(2, 2, sr, bs);
                ir->prepareToPlay(sr, bs);
                if (!ir->loadIR(juce::File(path)))
                {
                    fprintf(stderr, "[LoadPreset] IR load failed: %s\n", path.toRawUTF8());
                    continue;
                }
                processor = std::move(ir);
            }
            else continue;

            int slotId = liveEngine->getSignalChain().addProcessor(
                std::move(processor),
                (ProcessorSlot::Type)type,
                name, path);

            if (bypassed && slotId >= 0)
                liveEngine->getSignalChain().setBypass(slotId, true);

            // Stereo routing (St-1). Absent keys read back as 0 (= default), so
            // mono presets restore exactly as before.
            if (slotId >= 0)
            {
                if (slotObj->hasProperty("pan"))
                    liveEngine->getSignalChain().setPan(slotId, (float)(double)slotObj->getProperty("pan"));
                if (slotObj->hasProperty("branch"))
                    liveEngine->getSignalChain().setBranch(slotId, (int)slotObj->getProperty("branch"));
                if (slotObj->hasProperty("postGain"))
                    liveEngine->getSignalChain().setPostGain(slotId, (float)(double)slotObj->getProperty("postGain"));
                if (slotObj->hasProperty("branchSrc"))
                    liveEngine->getSignalChain().setBranchSrc(slotId, (int)slotObj->getProperty("branchSrc"));
            }

            // Restore processor state (JUCE-format base64; IR/NAM slots also
            // accept standard base64 — see decodeStateBlob: their plugin-
            // emitted JSON states were silently dropped before, so IR stages
            // never got their per-stage gain).
            if (stateB64.isNotEmpty() && slotId >= 0)
            {
                const bool allowStandard = type == (int)ProcessorSlot::Type::IR
                                        || type == (int)ProcessorSlot::Type::NAM;
                juce::MemoryBlock state;
                if (decodeStateBlob(stateB64, state, allowStandard))
                {
                    // Through the class's own synchronized API (deep-read 9) --
                    // no more const_cast around setSlotState's locking.
                    liveEngine->getSignalChain().setSlotState(slotId, state);
                }
            }

            slotsLoaded_++;
        }

        success_ = true;
        generation_ = slopsmith::addon::bumpChainGeneration();  // still under chainLock
    }

    void OnOK() override
    {
        auto obj = Napi::Object::New(Env());
        obj.Set("success", success_);
        obj.Set("slotsLoaded", slotsLoaded_);
        obj.Set("chainGeneration", (double) generation_);
        if (!success_) obj.Set("error", error_);
        deferred_.Resolve(obj);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::string presetJson_;
    uint64_t generation_ = 0;
    bool success_ = false;
    std::string error_;
    int slotsLoaded_ = 0;
};

Napi::Value LoadPreset(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    auto liveEngine = snapshotEngine();

    if (!liveEngine || info.Length() < 1) {
        auto obj = Napi::Object::New(env);
        obj.Set("success", false);
        obj.Set("error", "No engine or missing argument");
        deferred.Resolve(obj);
        return deferred.Promise();
    }

    // Validate + read the argument BEFORE arming the barrier: with a
    // non-string arg, As<Napi::String>() throws (JS TypeError / C++
    // exception), and anything thrown between begin and the worker taking
    // ownership would leak the barrier and block editor opens forever.
    if (!info[0].IsString())
    {
        auto obj = Napi::Object::New(env);
        obj.Set("success", false);
        obj.Set("error", "preset must be a JSON string");
        deferred.Resolve(obj);
        return deferred.Promise();
    }
    auto json = info[0].As<Napi::String>().Utf8Value();

    // Arm the rebuild barrier BEFORE editor teardown: between closeAll…()
    // returning and the queued worker acquiring chainMutationMutex, nothing
    // else stops OpenPluginEditor from opening a fresh editor whose processor
    // the worker is about to free (#56). The barrier gates editor opens for
    // the whole teardown+rebuild window; the worker releases it on every
    // Execute() exit path. Nothing between here and Queue() can throw: the
    // teardown-failure path below releases explicitly, and the worker's
    // BarrierRelease guard covers every Execute() exit.
    slopsmith::addon::beginChainRebuild();

    // Tear down any open in-process editor windows NOW, on the N-API/main
    // thread, before the AsyncWorker frees the chain's processors on a libuv
    // worker (#56). Doing it here — not inside LoadPresetWorker::Execute — keeps
    // JUCE GUI teardown off the worker thread: on macOS this thread IS the
    // message thread (inline teardown); on Linux/Windows closeAllPluginEditor-
    // Windows() posts to the dedicated JUCE message thread and blocks. Either
    // way editors are destroyed before Execute() clears the chain.
    if (!closeAllPluginEditorWindows())
    {
        // Teardown refused or timed out: an editor may still be alive and
        // bound to a chain processor. Clearing/rebuilding now would free that
        // processor under the live editor — the documented UAF. Abort the
        // load instead of proceeding.
        slopsmith::addon::endChainRebuild();
        auto obj = Napi::Object::New(env);
        obj.Set("success", false);
        obj.Set("error", "editor teardown did not complete; preset load aborted");
        deferred.Resolve(obj);
        return deferred.Promise();
    }

    auto worker = new LoadPresetWorker(env, deferred, std::move(json));
    worker->Queue();
    return deferred.Promise();
}


} // namespace slopsmith::addon
