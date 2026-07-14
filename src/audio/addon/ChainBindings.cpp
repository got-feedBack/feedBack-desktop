// Signal-chain slot/state/preset bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
// binding split). Registered by NodeAddon's export table via Bindings.h.

#include "Bindings.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "ChainOps.h"
#include "EditorWindows.h"
#include "../AudioEngine.h"
#include "../VSTHost.h"
#include "../VSTTrace.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace slopsmith::addon {

// ── Signal Chain Management ──────────────────────────────────────────────────

// Pending in-process loads: each LoadVSTWorker / LoadPresetWorker that's
// currently blocked on `done->wait()` registers its event here. doShutdown
// signals them all so the workers unblock and return a clean "cancelled"
// error instead of hanging forever when the JUCE message thread is about
// to be stopped (and any unfired callback would never arrive).
Napi::Value RemoveProcessor(const Napi::CallbackInfo& info)
{
    // Typed extractors (addon/NapiHelpers.h): NaN/Inf slot ids used to coerce
    // to slot 0 and mutate the wrong slot (deep-read §2) — now a clean no-op.
    auto liveEngine = snapshotEngine();
    const auto slotId = slopsmith::addon::argSlotId(info, 0);
    if (liveEngine && slotId)
    {
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        liveEngine->getSignalChain().removeProcessor(*slotId);
        slopsmith::addon::bumpChainGeneration();
    }
    return info.Env().Undefined();
}

Napi::Value MoveProcessor(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    const auto from = slopsmith::addon::argSlotId(info, 0);
    const auto to = slopsmith::addon::argSlotId(info, 1);
    if (liveEngine && from && to)
    {
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        liveEngine->getSignalChain().moveProcessor(*from, *to);
        slopsmith::addon::bumpChainGeneration();
    }
    return info.Env().Undefined();
}

Napi::Value SetBypass(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    const auto slotId = slopsmith::addon::argSlotId(info, 0);
    const auto bypassed = slopsmith::addon::argBool(info, 1);
    if (liveEngine && slotId && bypassed)
        liveEngine->getSignalChain().setBypass(*slotId, *bypassed);
    return info.Env().Undefined();
}

// Destroy every open in-process plugin editor window on the message thread and
// block until done. MUST run before any path that frees slot processors
// (ClearChain, LoadPreset's chain rebuild, engine teardown): an editor window
// owns an AudioProcessorEditor bound to its slot's processor, so if the
// processor is freed first the editor's next timer/paint callback dereferences
// freed memory (use-after-free → DEP-execute crash seconds after pause;
// feedBack-desktop#56). Lives in addon/EditorWindows now.

Napi::Value ClearChain(const Napi::CallbackInfo& info)
{
    // Gate editor opens for the whole teardown+clear window (see the rebuild
    // barrier in ChainOps.h): without it, an editor opened between the
    // teardown below and the clear acquiring the mutex would point at a
    // processor the clear is about to free.
    slopsmith::addon::beginChainRebuild();
    struct BarrierRelease {
        ~BarrierRelease() { slopsmith::addon::endChainRebuild(); }
    } barrierRelease;

    // Tear editors down before their processors are freed just below (#56).
    if (!closeAllPluginEditorWindows())
    {
        // Teardown refused/timed out: an editor may still be bound to a chain
        // processor. Clearing now would free it under the live editor — the
        // documented UAF. Skip the clear; the caller can retry.
        fprintf(stderr, "[audio-native] clearChain: editor teardown did not complete; "
                        "chain left untouched\n");
        return info.Env().Undefined();
    }
    if (auto liveEngine = snapshotEngine())
    {
        // Serialized with the async chain workers (deep-read 1). May block
        // briefly behind an in-flight preset/VST load -- that wait IS the fix
        // for the interleaved clear-vs-rebuild corruption.
        std::lock_guard<std::mutex> chainLock(slopsmith::addon::chainMutationMutex());
        liveEngine->getSignalChain().clear();
        slopsmith::addon::bumpChainGeneration();
    }
    return info.Env().Undefined();
}

// Stereo routing (St-1). setPan(slotId, -1..+1); setBranch(slotId, 0=trunk/>=1).
Napi::Value SetPan(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        const auto slotId = slopsmith::addon::argSlotId(info, 0);
        const auto pan = slopsmith::addon::argFiniteFloat(info, 1);
        if (slotId && pan) liveEngine->getSignalChain().setPan(*slotId, *pan);
    }
    return info.Env().Undefined();
}

Napi::Value SetPostGain(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        const auto slotId = slopsmith::addon::argSlotId(info, 0);
        const auto gain = slopsmith::addon::argFiniteFloat(info, 1);
        if (slotId && gain) liveEngine->getSignalChain().setPostGain(*slotId, *gain);
    }
    return info.Env().Undefined();
}

Napi::Value SetBranch(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        const auto slotId = slopsmith::addon::argSlotId(info, 0);
        const auto branch = slopsmith::addon::argInt(info, 1);
        if (slotId && branch) liveEngine->getSignalChain().setBranch(*slotId, *branch);
    }
    return info.Env().Undefined();
}

// setBranchSrc(slotId, 0=both/1=L/2=R): channel a branch reads from the split.
Napi::Value SetBranchSrc(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2)
    {
        const auto slotId = slopsmith::addon::argSlotId(info, 0);
        const auto branchSrc = slopsmith::addon::argInt(info, 1, 0, 2);
        if (slotId && branchSrc) liveEngine->getSignalChain().setBranchSrc(*slotId, *branchSrc);
    }
    return info.Env().Undefined();
}

// ── Chain State ───────────────────────────────────────────────────────────────

// Monotonic chain-mutation counter (TLC phase 7): JS-side chain owners (the
// audio-effects executor) compare this against the generation their load
// returned to detect that another writer changed the chain under them.
Napi::Value GetChainGeneration(const Napi::CallbackInfo& info)
{
    return Napi::Number::New(info.Env(), (double) slopsmith::addon::currentChainGeneration());
}

Napi::Value GetChainState(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto result = Napi::Array::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        auto slots = liveEngine->getSignalChain().getAllSlots();
        for (int i = 0; i < slots.size(); ++i)
        {
            auto obj = Napi::Object::New(env);
            obj.Set("id", slots[i]->id);
            obj.Set("type", (int)slots[i]->type);
            obj.Set("name", slots[i]->name.toStdString());
            obj.Set("path", slots[i]->path.toStdString());
            obj.Set("bypassed", slots[i]->bypassed);
            obj.Set("pan", slots[i]->pan);
            obj.Set("branch", slots[i]->branch);
            obj.Set("branchSrc", slots[i]->branchSrc);
            obj.Set("postGain", slots[i]->postGain);
            obj.Set("hasEditor", slots[i]->processor && slots[i]->processor->hasEditor());
            result.Set((uint32_t)i, obj);
        }
    }

    return result;
}

// ── Parameters ────────────────────────────────────────────────────────────────

Napi::Value GetParameters(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1) return Napi::Array::New(env);

    int slotId = info[0].As<Napi::Number>().Int32Value();
    auto params = liveEngine->getSignalChain().getParameters(slotId);
    auto result = Napi::Array::New(env, params.size());

    for (int i = 0; i < params.size(); ++i)
    {
        auto obj = Napi::Object::New(env);
        obj.Set("index", params[i].index);
        obj.Set("name", params[i].name.toStdString());
        obj.Set("value", params[i].value);
        obj.Set("label", params[i].label.toStdString());
        obj.Set("text", params[i].text.toStdString());
        result.Set((uint32_t)i, obj);
    }

    return result;
}

Napi::Value SetParameter(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    const auto slotId = slopsmith::addon::argSlotId(info, 0);
    const auto paramIdx = slopsmith::addon::argSlotId(info, 1);
    const auto value = slopsmith::addon::argFiniteFloat(info, 2);
    if (liveEngine && slotId && paramIdx && value)
        liveEngine->getSignalChain().setParameter(*slotId, *paramIdx, *value);
    return info.Env().Undefined();
}

// Restore a VST slot's full state from a base64 getStateInformation() blob.
Napi::Value SetSlotState(const Napi::CallbackInfo& info)
{
    // Type-guard both args (NAPI_DISABLE_CPP_EXCEPTIONS): a malformed IPC
    // payload is a clean no-op rather than a hard addon failure.
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsString())
    {
        int slotId = info[0].As<Napi::Number>().Int32Value();
        auto base64 = info[1].As<Napi::String>().Utf8Value();
        const auto* slot = liveEngine->getSignalChain().getSlot(slotId);
        const bool allowStandard = slot != nullptr
            && (slot->type == ProcessorSlot::Type::IR
                || slot->type == ProcessorSlot::Type::NAM);
        juce::MemoryBlock mb;
        if (decodeStateBlob(juce::String(base64), mb, allowStandard))
            liveEngine->getSignalChain().setSlotState(slotId, mb);
    }
    return info.Env().Undefined();
}

// ── Presets ───────────────────────────────────────────────────────────────────

Napi::Value SavePreset(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();
    auto json = liveEngine->getSignalChain().savePreset();
    return Napi::String::New(env, json.toStdString());
}

Napi::Value SetMultiBypass(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsArray())
        return Napi::Boolean::New(env, false);

    auto arr = info[0].As<Napi::Array>();
    juce::Array<std::pair<int, bool>> changes;

    for (uint32_t i = 0; i < arr.Length(); i++)
    {
        // Per-item type guards (deep-read §2): a malformed entry is skipped
        // instead of coercing NaN to slot 0.
        auto itemVal = arr.Get(i);
        if (!itemVal.IsObject()) continue;
        auto item = itemVal.As<Napi::Object>();
        auto slotVal = item.Get("slotId");
        auto bypVal = item.Get("bypassed");
        if (!slotVal.IsNumber() || !bypVal.IsBoolean()) continue;
        const double raw = slotVal.As<Napi::Number>().DoubleValue();
        if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0.0 || raw > 4096.0) continue;
        changes.add({ (int) raw, bypVal.As<Napi::Boolean>().Value() });
    }

    liveEngine->getSignalChain().setMultiBypass(changes);
    return Napi::Boolean::New(env, true);
}


} // namespace slopsmith::addon
