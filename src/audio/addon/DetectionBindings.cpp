// Pitch detection + source-indexed bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
// binding split). Registered by NodeAddon's export table via Bindings.h.

#include "Bindings.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "ChainOps.h"
#include "../AudioEngine.h"
#include "../VSTHost.h"
#include "../VSTTrace.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

namespace slopsmith::addon {

// Validate a JS source-id argument and return the live source, or nullptr if it is
// missing / not a Number / not a FINITE INTEGER / out of range. The TS bridge already
// validates, but the addon must fail soft on its own: Int32Value() silently coerces
// NaN/Infinity into a valid index (NaN -> 0), which would let a malformed id hit a
// real source (e.g. the default source 0). getSource() does the final
// [0, kMaxSources) + active check; the 4096 guard keeps the cast well-defined.
SourceChain* getValidatedSource(AudioEngine* eng, const Napi::CallbackInfo& info, size_t argIndex)
{
    if (eng == nullptr || argIndex >= info.Length() || ! info[argIndex].IsNumber())
        return nullptr;
    const double raw = info[argIndex].As<Napi::Number>().DoubleValue();
    if (! std::isfinite(raw) || raw != std::floor(raw) || raw < 0.0 || raw > 4096.0)
        return nullptr;
    return eng->getSource((int) raw);
}

// ── Pitch Detection (polled) ──────────────────────────────────────────────────

// Load the Basic Pitch ONNX model for the polyphonic ML note detector.
// Called once at startup by audio-bridge.ts with the bundled model path.
// Never throws. Returns "is ML note detection available after this call" —
// a model is loaded with a valid contract. A missing/invalid file does NOT
// tear down an already-loaded model, so it can still return true; it returns
// false when the engine isn't ready or ONNX support isn't compiled in, and
// the engine then keeps using the YIN PitchDetector / ChordScorer
// (Constitution VII).
Napi::Value LoadNoteModel(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsString())
        return Napi::Boolean::New(env, false);

    const auto path = info[0].As<Napi::String>().Utf8Value();
    const bool ok = liveEngine->loadNoteModel(juce::File(juce::String(path)));
    return Napi::Boolean::New(env, ok);
}

// Whether the ML note detector is active (ONNX support compiled in AND a
// model loaded). Lets the renderer / tests tell the ML path from the YIN
// fallback without inferring it from behaviour.
Napi::Value IsMlNoteDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Report readiness, not just model-loaded: the engine only routes
    // getPitchDetection()/scoreChord() to ML once the detector has published
    // its first snapshot (isReady()). Reporting true during the cold-start
    // window would tell the renderer "ML active" while it's still getting the
    // YIN fallback.
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(env,
        liveEngine && liveEngine->hasMlNoteDetector()
              && liveEngine->getMlNoteDetector().isReady());
}

// Raw polyphonic transcription from the ML note detector — the full set of
// currently-active pitches, not just the dominant one. Returns
// `{ notes: [{ midi, confidence, onsetMs, onsetSeq }], sampleRate }`, or null when the ML
// detector isn't active (no model / ONNX support) so the renderer can feature-
// detect and fall back. Never throws.
Napi::Value DetectNotes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Gate on isReady(): the contract is that callers get null whenever the
    // ML detector isn't actively producing notes. isReady() is false with no
    // model, after a device stop, and during the cold-start window before the
    // first inference publishes — so the renderer feature-detects correctly
    // and falls back instead of consuming an empty ML stream.
    auto liveEngine = snapshotEngine();
    if (!liveEngine || !liveEngine->getMlNoteDetector().isReady())
        return env.Null();

    const auto active = liveEngine->getMlNoteDetector().getActiveNotes();
    auto notesArr = Napi::Array::New(env, active.size());
    for (size_t i = 0; i < active.size(); ++i)
    {
        auto entry = Napi::Object::New(env);
        entry.Set("midi", active[i].midi);
        entry.Set("confidence", active[i].confidence);
        // Milliseconds since this pitch's onset — lets the renderer back-date
        // a detection to the true onset instead of poll time.
        entry.Set("onsetMs", active[i].onsetAgeMs);
        // Monotonic per-pitch onset counter — a change means a new note was
        // struck, so the renderer can consume onsets as discrete events.
        entry.Set("onsetSeq", active[i].onsetSeq);
        notesArr.Set((uint32_t) i, entry);
    }

    auto obj = Napi::Object::New(env);
    obj.Set("notes", notesArr);
    // Normalise the sample rate: getCurrentSampleRate() is 0 when no audio
    // device is active — hand the renderer a sane positive value so its
    // Hz/time math can't divide by zero.
    const double sr = liveEngine->getCurrentSampleRate();
    obj.Set("sampleRate", sr > 0.0 ? sr : 48000.0);
    return obj;
}

Napi::Value GetPitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        // getActiveDetection() returns the polyphonic ML detector's dominant
        // pitch when a Basic Pitch model is loaded, else the YIN detector's
        // latest result — same shape either way, so the plugin is unchanged.
        auto det = liveEngine->getActiveDetection();
        obj.Set("frequency", det.frequency);
        obj.Set("confidence", det.confidence);
        obj.Set("midiNote", det.midiNote);
        obj.Set("cents", det.cents);
        obj.Set("noteName", det.noteName.toStdString());
    }
    else
    {
        obj.Set("frequency", -1.0);
        obj.Set("confidence", 0.0);
        obj.Set("midiNote", -1);
        obj.Set("cents", 0.0);
        obj.Set("noteName", "");
    }

    return obj;
}

Napi::Value GetRawPitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        // Always the raw YIN detection — bypasses the ML preference so frequency
        // stays continuous (sub-Hz) and cents stays real even with a model loaded.
        // Backs the tuner's audio:getRawPitch endpoint.
        auto det = liveEngine->getRawPitchDetection();
        obj.Set("frequency", det.frequency);
        obj.Set("confidence", det.confidence);
        obj.Set("midiNote", det.midiNote);
        obj.Set("cents", det.cents);
        obj.Set("noteName", det.noteName.toStdString());
    }
    else
    {
        obj.Set("frequency", -1.0);
        obj.Set("confidence", 0.0);
        obj.Set("midiNote", -1);
        obj.Set("cents", 0.0);
        obj.Set("noteName", "");
    }

    return obj;
}

Napi::Value GetRawAudioFrame(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();

    // Optional sample count; defaults to AudioEngine::getRawAudioFrame's 4096.
    // The engine clamps anything above its ring capacity.
    int numSamples = 4096;
    if (info.Length() > 0 && info[0].IsNumber())
        numSamples = info[0].As<Napi::Number>().Int32Value();

    if (!liveEngine || numSamples <= 0)
        return Napi::Float32Array::New(env, 0);

    // Post-gate mono snapshot for the tuner's own pitch pipeline. Returns a
    // Float32Array of the most-recent N samples (left-zero-padded on cold start).
    auto frame = liveEngine->getRawAudioFrame(numSamples);
    auto out = Napi::Float32Array::New(env, frame.size());
    float* dst = out.Data();
    for (size_t i = 0; i < frame.size(); ++i)
        dst[i] = frame[i];
    return out;
}

// Score a polyphonic chord against the engine's most recent input
// samples. Renderer (notedetect plugin's matchNotes chord branch)
// supplies the chord context — chart notes plus tuning/arrangement
// metadata — and gets back a `{score, hitStrings, totalStrings, isHit,
// results[]}` object identical in shape to what the JS implementation
// produced. Audio never crosses the N-API boundary, which is the
// whole reason for moving the math here: constitution II says audio
// analysis lives in JUCE, and this is the missing piece.
//
// Request shape. Fields marked `required` must be present and
// internally consistent — the C++ scorer fails closed (all-miss
// result with one entry per requested note) when the validation
// invariants don't hold, rather than silently substituting defaults.
// {
//   notes: [{ s, f, ho?, po?, b?, sl?, hm? }, ...],
//                                   // required, each `s` must be in [0, stringCount)
//   arrangement?: 'guitar'|'bass',  // default 'guitar' — must be one of these two strings
//   stringCount?: number,           // default 6 — must match the (arrangement, stringCount)
//                                   //  table: bass{4,5} or guitar{6,7,8}
//   offsets: number[],              // required, length must equal stringCount.
//                                   //  Pass an array of zeros for standard tuning;
//                                   //  the default of `stringCount = 6` only works
//                                   //  if you supply 6 offsets.
//   numSamples?: number,            // analysis window (default 4096, capped at the
//                                   //  engine input-ring capacity, currently 8192)
//   capo?: number,                  // default 0
//   pitchCheckCents?: number,       // 0 = energy-only chord check (default 0)
//   minHitRatio?: number,           // default 0.6
//   bypassMl?: boolean,             // force the DSP band-energy scorer even
//                                   //  when an ML model is loaded (default false)
//   harmonicVerify?: boolean,       // score each note by harmonic-comb energy
//                                   //  (f,2f..5f vs the floor between) instead
//                                   //  of band-energy/total (default false)
//   harmonicSnr?: number,           // min harmonic-to-floor ratio for a hit
//                                   //  when harmonicVerify is set (default 3.0)
//   fundamentalRatio?: number,      // fundamental-presence gate: reject when
//                                   //  f0 peak < ratio*strongest partial; lower
//                                   //  for bass, <=0 disables (default 0.20)
// }
// Shared core: parse `reqObj` into a ChordScorer::Request and score it against
// `target`'s input ring. `target` is sources[0] for the legacy scoreChord and
// getSource(id) for the source-indexed scoreSourceChord.
Napi::Value scoreChordCore(Napi::Env env, Napi::Object reqObj, SourceChain* target)
{
    // Hard caps on caller-controlled array lengths. The scorer's
    // (arrangement, stringCount) validation only accepts up to 8
    // strings; chord-notes have a natural ceiling at the same value
    // (one per string). 32 is a generous headroom that still bounds
    // worst-case allocations the renderer could trigger over IPC —
    // without these limits, a malformed/malicious payload claiming a
    // gigantic JS array length would force a multi-GB reserve before
    // the scorer's own validation rejected the request. A request
    // that exceeds either cap is treated as outright malformed and
    // returns the "no chord requested" failure shape (totalStrings=0);
    // every other validation failure goes through the all-miss path
    // below so results[] stays in lockstep with notes[].
    static constexpr uint32_t kMaxOffsets = 32;
    static constexpr uint32_t kMaxNotes = 32;

    auto noRequestFailure = [&env]() {
        auto failure = Napi::Object::New(env);
        failure.Set("score", 0.0);
        failure.Set("hitStrings", 0);
        failure.Set("totalStrings", 0);
        failure.Set("isHit", false);
        failure.Set("results", Napi::Array::New(env, 0));
        return failure;
    };

    // Capture the notes array up front so every downstream failure
    // path can build a per-note all-miss result aligned 1:1 with the
    // caller's notes[]. Pre-cap check happens before we even read the
    // length into the helper to prevent a payload claiming an enormous
    // length from forcing the helper to allocate a huge results array.
    Napi::Value notesVal = reqObj.Has("notes") ? reqObj.Get("notes") : env.Null();
    if (!notesVal.IsArray()) return noRequestFailure();
    auto notesArr = notesVal.As<Napi::Array>();
    if (notesArr.Length() > kMaxNotes) return noRequestFailure();
    const uint32_t noteCount = notesArr.Length();

    // All-miss result aligned with the caller's notes[]. Walks the
    // original JS array so the per-note `s` / `f` echo back in the
    // result even when the request fails validation (lets the renderer
    // distinguish "this string missed" from "this string wasn't sent").
    // Used by every failure path below except the cap/no-notes case
    // above, which doesn't have a coherent notes[] to mirror.
    auto buildAllMiss = [&]() {
        auto resultsArr = Napi::Array::New(env, noteCount);
        for (uint32_t i = 0; i < noteCount; ++i)
        {
            int s = -1, f = -1;
            auto v = notesArr.Get(i);
            if (v.IsObject())
            {
                auto o = v.As<Napi::Object>();
                if (o.Has("s") && o.Get("s").IsNumber())
                    s = o.Get("s").As<Napi::Number>().Int32Value();
                if (o.Has("f") && o.Get("f").IsNumber())
                    f = o.Get("f").As<Napi::Number>().Int32Value();
            }
            auto entry = Napi::Object::New(env);
            entry.Set("s", s);
            entry.Set("f", f);
            entry.Set("hit", false);
            entry.Set("bandEnergy", 0.0);
            entry.Set("centsDiff", env.Null());
            entry.Set("centsError", env.Null());
            resultsArr.Set(i, entry);
        }
        auto out = Napi::Object::New(env);
        out.Set("score", 0.0);
        out.Set("hitStrings", 0);
        out.Set("totalStrings", (int) noteCount);
        out.Set("isHit", false);
        out.Set("results", resultsArr);
        return out;
    };

    ChordScorer::Request req;
    if (reqObj.Has("numSamples") && reqObj.Get("numSamples").IsNumber())
        req.numSamples = reqObj.Get("numSamples").As<Napi::Number>().Int32Value();
    if (reqObj.Has("arrangement") && reqObj.Get("arrangement").IsString())
        req.arrangement = reqObj.Get("arrangement").As<Napi::String>().Utf8Value();
    if (reqObj.Has("stringCount") && reqObj.Get("stringCount").IsNumber())
        req.stringCount = reqObj.Get("stringCount").As<Napi::Number>().Int32Value();
    if (reqObj.Has("capo") && reqObj.Get("capo").IsNumber())
        req.capo = reqObj.Get("capo").As<Napi::Number>().Int32Value();
    if (reqObj.Has("pitchCheckCents") && reqObj.Get("pitchCheckCents").IsNumber())
        req.pitchCheckCents = reqObj.Get("pitchCheckCents").As<Napi::Number>().FloatValue();
    if (reqObj.Has("minHitRatio") && reqObj.Get("minHitRatio").IsNumber())
        req.minHitRatio = reqObj.Get("minHitRatio").As<Napi::Number>().FloatValue();
    if (reqObj.Has("bypassMl") && reqObj.Get("bypassMl").IsBoolean())
        req.bypassMl = reqObj.Get("bypassMl").As<Napi::Boolean>().Value();
    if (reqObj.Has("harmonicVerify") && reqObj.Get("harmonicVerify").IsBoolean())
        req.harmonicVerify = reqObj.Get("harmonicVerify").As<Napi::Boolean>().Value();
    if (reqObj.Has("harmonicSnr") && reqObj.Get("harmonicSnr").IsNumber())
        req.harmonicSnr = reqObj.Get("harmonicSnr").As<Napi::Number>().FloatValue();
    if (reqObj.Has("fundamentalRatio") && reqObj.Get("fundamentalRatio").IsNumber())
    {
        // Drop NaN/Inf: a non-finite ratio poisons the fundamental-presence
        // gate (fundMag >= NaN is always false -> every note false-rejected).
        // Keep the safe 0.20 default instead.
        const float v = reqObj.Get("fundamentalRatio").As<Napi::Number>().FloatValue();
        if (std::isfinite(v)) req.fundamentalRatio = v;
    }

    if (reqObj.Has("offsets") && reqObj.Get("offsets").IsArray())
    {
        auto arr = reqObj.Get("offsets").As<Napi::Array>();
        if (arr.Length() > kMaxOffsets) return noRequestFailure();
        req.tuningOffsets.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto v = arr.Get(i);
            // Tuning offsets materially shift expected pitch — silently
            // substituting 0 for a missing/non-numeric entry would
            // produce confidently wrong scores. Fail closed with the
            // per-note all-miss shape so the renderer sees the right
            // results[] length even when the request is malformed.
            if (!v.IsNumber()) return buildAllMiss();
            req.tuningOffsets.push_back(v.As<Napi::Number>().Int32Value());
        }
    }

    req.notes.reserve(noteCount);
    for (uint32_t i = 0; i < noteCount; ++i)
    {
        auto v = notesArr.Get(i);
        // For malformed entries (non-object, or missing/non-numeric
        // s/f) push a sentinel Note with string = -1. This keeps
        // req.notes.size() in lockstep with the incoming notes[]
        // length AND guarantees ChordScorer's range check
        // (`n.string < 0 || n.string >= stringCount`) trips on the
        // sentinel — yielding the same all-miss fail-closed result
        // the shape contract advertises, never a false hit on the
        // default low-string position.
        ChordScorer::Note n{};
        n.string = -1;
        n.fret = -1;
        if (!v.IsObject())
        {
            req.notes.push_back(n);
            continue;
        }
        auto noteObj = v.As<Napi::Object>();
        const bool hasS = noteObj.Has("s") && noteObj.Get("s").IsNumber();
        const bool hasF = noteObj.Has("f") && noteObj.Get("f").IsNumber();
        if (!hasS || !hasF)
        {
            req.notes.push_back(n);
            continue;
        }
        n.string = noteObj.Get("s").As<Napi::Number>().Int32Value();
        n.fret = noteObj.Get("f").As<Napi::Number>().Int32Value();
        // Technique flags are truthy/falsy in JS; coerce to bool
        // here so an unset value cleanly becomes false.
        auto truthy = [&noteObj](const char* key) {
            if (!noteObj.Has(key)) return false;
            auto val = noteObj.Get(key);
            return val.ToBoolean().Value();
        };
        n.hammerOn = truthy("ho");
        n.pullOff = truthy("po");
        n.bend = truthy("b");
        n.slide = truthy("sl");
        n.harmonic = truthy("hm");
        req.notes.push_back(n);
    }

    auto result = target->scoreChord(req);

    auto out = Napi::Object::New(env);
    out.Set("score", result.score);
    out.Set("hitStrings", result.hitStrings);
    out.Set("totalStrings", result.totalStrings);
    out.Set("isHit", result.isHit);
    auto resultsArr = Napi::Array::New(env, result.results.size());
    for (size_t i = 0; i < result.results.size(); ++i)
    {
        const auto& r = result.results[i];
        auto entry = Napi::Object::New(env);
        entry.Set("s", r.string);
        entry.Set("f", r.fret);
        entry.Set("hit", r.hit);
        entry.Set("bandEnergy", r.bandEnergy);
        // Mirror the JS result shape: when cents weren't measured the
        // fields are present-but-null so the renderer can distinguish
        // "no pitch check ran" (null) from "pitch check said 0"
        // (numeric 0).
        if (r.hasCents)
        {
            entry.Set("centsDiff", r.centsDiff);
            entry.Set("centsError", r.centsError);
        }
        else
        {
            entry.Set("centsDiff", env.Null());
            entry.Set("centsError", env.Null());
        }
        resultsArr.Set(i, entry);
    }
    out.Set("results", resultsArr);
    return out;
}

// Legacy: scoreChord(req) — targets sources[0]. Backward-compatible.
Napi::Value ScoreChord(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto noRequestFailure = [&env]() {
        auto failure = Napi::Object::New(env);
        failure.Set("score", 0.0);
        failure.Set("hitStrings", 0);
        failure.Set("totalStrings", 0);
        failure.Set("isHit", false);
        failure.Set("results", Napi::Array::New(env, 0));
        return failure;
    };
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return noRequestFailure();
    return scoreChordCore(env, info[0].As<Napi::Object>(), liveEngine->getSource(0));
}

// Source-indexed: scoreSourceChord(sourceId, req). Bad id / payload -> the
// same "no chord requested" failure shape (totalStrings=0).
Napi::Value ScoreSourceChord(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto noRequestFailure = [&env]() {
        auto failure = Napi::Object::New(env);
        failure.Set("score", 0.0);
        failure.Set("hitStrings", 0);
        failure.Set("totalStrings", 0);
        failure.Set("isHit", false);
        failure.Set("results", Napi::Array::New(env, 0));
        return failure;
    };
    if (!liveEngine || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject())
        return noRequestFailure();
    SourceChain* target = getValidatedSource(liveEngine.get(), info, 0);
    if (!target) return noRequestFailure();
    return scoreChordCore(env, info[1].As<Napi::Object>(), target);
}

// ── Multi-input source management bridge ─────────────────────────────────────
// A source is one independent input chain (own arrangement chart, detection,
// scoring, tone, monitor). sources[0] always exists. The renderer adds a source
// per extra player, binds it to an input channel, and drives its scoring via the
// *Source* methods below; the legacy un-suffixed methods keep targeting source 0.

// addSource(inputChannel?) -> sourceId (number), or -1 if the pool is full.
Napi::Value AddSource(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Number::New(env, -1);
    int channel = -1;  // default: mono mix of the first pair
    if (info.Length() > 0 && info[0].IsNumber())
        channel = info[0].As<Napi::Number>().Int32Value();
    int deviceKey = 0;  // default: primary input device
    if (info.Length() > 1 && info[1].IsNumber())
    {
        const int k = info[1].As<Napi::Number>().Int32Value();
        if (k >= 0) deviceKey = k;  // negatives ignored → primary
    }
    // deviceKey != 0 opens an extra input AudioIODevice — device lifecycle,
    // so it must run on the JUCE message thread (see runDeviceLifecycleOp).
    auto sourceId = std::make_shared<int>(-1);
    if (!runDeviceLifecycleOp([liveEngine, channel, deviceKey, sourceId] {
            *sourceId = liveEngine->addSource(channel, deviceKey);
        }))
        return Napi::Number::New(env, -1);
    return Napi::Number::New(env, *sourceId);
}

// removeSource(sourceId) -> boolean. sources[0] cannot be removed.
Napi::Value RemoveSource(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsNumber())
        return Napi::Boolean::New(env, false);
    // May close the source's extra input AudioIODevice — message thread hop
    // for the same reason as AddSource.
    const int sourceId = info[0].As<Napi::Number>().Int32Value();
    auto removed = std::make_shared<bool>(false);
    if (!runDeviceLifecycleOp([liveEngine, sourceId, removed] {
            *removed = liveEngine->removeSource(sourceId);
        }))
        return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, *removed);
}

// listSources() -> [{ id, inputChannel, active }]. Null on a missing engine.
Napi::Value ListSources(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();
    const auto sources = liveEngine->listSources();
    auto arr = Napi::Array::New(env, sources.size());
    for (size_t i = 0; i < sources.size(); ++i)
    {
        auto entry = Napi::Object::New(env);
        entry.Set("id", sources[i].id);
        entry.Set("inputChannel", sources[i].inputChannel);
        entry.Set("deviceKey", sources[i].deviceKey);
        entry.Set("active", sources[i].active);
        arr.Set((uint32_t) i, entry);
    }
    return arr;
}

// listInputDevices() -> [{ typeName, name }]. Every available capture device the
// renderer can bind to an additional engine input via bindInputDevice. Null on a
// missing engine.
Napi::Value ListInputDevices(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();
    const auto devices = liveEngine->getBindableInputDevices();
    auto arr = Napi::Array::New(env);
    uint32_t n = 0;
    for (const auto& d : devices)
    {
        auto entry = Napi::Object::New(env);
        entry.Set("typeName", d.typeName.toStdString());
        entry.Set("name", d.name.toStdString());
        arr.Set(n++, entry);
    }
    return arr;
}

// bindInputDevice(deviceKey, deviceName) -> "" on success, else an error string.
// Opens an ADDITIONAL physical input device (deviceKey 1..N) so sources created
// with addSource(channel, deviceKey) capture from it at its own clock.
Napi::Value BindInputDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::String::New(env, "no engine");
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString())
        return Napi::String::New(env, "bindInputDevice(deviceKey:number, deviceName:string)");
    const int deviceKey = info[0].As<Napi::Number>().Int32Value();
    const std::string name = info[1].As<Napi::String>().Utf8Value();
    // Opens a physical AudioIODevice — message thread hop (runDeviceLifecycleOp).
    auto err = std::make_shared<juce::String>();
    if (!runDeviceLifecycleOp([liveEngine, deviceKey, name, err] {
            *err = liveEngine->bindInputDevice(deviceKey, name);
        }))
        return Napi::String::New(env,
            "bindInputDevice did not complete (message thread unavailable or timed out)");
    return Napi::String::New(env, err->toStdString());
}

// unbindInputDevice(deviceKey) -> boolean. Stops + releases the extra device.
Napi::Value UnbindInputDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsNumber())
        return Napi::Boolean::New(env, false);
    // Closes a physical AudioIODevice — message thread hop (runDeviceLifecycleOp).
    const int deviceKey = info[0].As<Napi::Number>().Int32Value();
    auto unbound = std::make_shared<bool>(false);
    if (!runDeviceLifecycleOp([liveEngine, deviceKey, unbound] {
            *unbound = liveEngine->unbindInputDevice(deviceKey);
        }))
        return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, *unbound);
}


} // namespace slopsmith::addon
