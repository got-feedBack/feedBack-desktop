// Gain/metering/MIDI/debug-logging bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
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
#include <string>

namespace slopsmith::addon {

// ── Gain ──────────────────────────────────────────────────────────────────────

Napi::Value SetGain(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 2) return env.Undefined();

    if (!info[0].IsString()) return env.Undefined();
    auto which = info[0].As<Napi::String>().Utf8Value();
    const auto valueOpt = slopsmith::addon::argFiniteFloat(info, 1);
    if (!valueOpt) return env.Undefined();  // engine clamps range; NaN/Inf rejected here
    const float value = *valueOpt;

    if (which == "input") liveEngine->setInputGain(value);
    else if (which == "output") liveEngine->setOutputGain(value);
    else if (which == "chain") liveEngine->setChainOutputGain(value);
    else if (which == "backing") liveEngine->setBackingVolume(value);

    return env.Undefined();
}

Napi::Value SetInputChannel(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setInputChannel(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

Napi::Value SetMonitorMute(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setMonitorMute(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

// setNoteDetectionEnabled(bool) -> undefined. Arms/suspends the polyphonic ML
// note-detection pipeline across all sources. The renderer (note_detect) calls
// this true only while a consumer actually reads ML notes (native-frame
// detection / non-verifier fallback) and false otherwise — the default
// harmonic-comb verifier path and the always-on home tuner leave ML suspended,
// so the engine runs no ONNX inference when nothing needs it.
Napi::Value SetNoteDetectionEnabled(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setMlNoteDetectionEnabled(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

Napi::Value SetMonitorMuteSuppressed(const Napi::CallbackInfo& info)
{
    // IsBoolean()-guarded so a mismatched renderer build / manual caller
    // passing a non-boolean is a clean no-op rather than a hard N-API failure
    // (NAPI_DISABLE_CPP_EXCEPTIONS is enabled). Mirrors SetNoiseGate's style.
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0 && info[0].IsBoolean())
        liveEngine->setMonitorMuteSuppressed(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

Napi::Value SetMonitorKill(const Napi::CallbackInfo& info)
{
    // IsBoolean()-guarded (fail-soft no-op on a downlevel/mismatched caller),
    // mirroring SetMonitorMuteSuppressed.
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0 && info[0].IsBoolean())
        liveEngine->setMonitorKill(info[0].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

Napi::Value SetNoiseGate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return env.Undefined();

    auto o = info[0].As<Napi::Object>();

    bool enabled = false;
    if (o.Has("enabled"))
    {
        auto v = o.Get("enabled");
        if (v.IsBoolean())
            enabled = v.As<Napi::Boolean>().Value();
        else if (v.IsNumber())
            enabled = v.As<Napi::Number>().DoubleValue() != 0.0;
    }

    float thresholdDb = -60.0f;
    if (o.Has("thresholdDb") && o.Get("thresholdDb").IsNumber())
        thresholdDb = (float)o.Get("thresholdDb").As<Napi::Number>().DoubleValue();

    float releaseMs = 100.0f;
    if (o.Has("releaseMs") && o.Get("releaseMs").IsNumber())
        releaseMs = (float)o.Get("releaseMs").As<Napi::Number>().DoubleValue();

    float depthDb = -60.0f;
    if (o.Has("depthDb") && o.Get("depthDb").IsNumber())
        depthDb = (float)o.Get("depthDb").As<Napi::Number>().DoubleValue();

    liveEngine->setNoiseGate(enabled, thresholdDb, releaseMs, depthDb);
    return env.Undefined();
}

Napi::Value SetTonePolish(const Napi::CallbackInfo& info)
{
    // Tone Polish — { enabled: bool }. Mirrors SetNoiseGate's defensive
    // shape so a mismatched renderer build / manual caller passing a
    // non-object is a clean no-op rather than a hard N-API failure
    // (NAPI_DISABLE_CPP_EXCEPTIONS).
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
        return env.Undefined();

    auto o = info[0].As<Napi::Object>();

    bool enabled = true;
    if (o.Has("enabled"))
    {
        auto v = o.Get("enabled");
        if (v.IsBoolean())
            enabled = v.As<Napi::Boolean>().Value();
        else if (v.IsNumber())
            enabled = v.As<Napi::Number>().DoubleValue() != 0.0;
    }

    liveEngine->setTonePolishEnabled(enabled);
    return env.Undefined();
}

Napi::Value IsMonitorMuted(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(info.Env(), liveEngine ? liveEngine->isMonitorMuted() : true);
}

// ── Metering (polled — read atomics) ──────────────────────────────────────────

Napi::Value GetLevels(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();

    if (liveEngine)
    {
        obj.Set("inputLevel", liveEngine->getInputLevel());
        obj.Set("outputLevel", liveEngine->getOutputLevel());
        obj.Set("inputPeak", liveEngine->getInputPeak());
        obj.Set("outputPeak", liveEngine->getOutputPeak());
    }
    else
    {
        obj.Set("inputLevel", 0.0);
        obj.Set("outputLevel", 0.0);
        obj.Set("inputPeak", 0.0);
        obj.Set("outputPeak", 0.0);
    }

    return obj;
}

// getSourceLevels(sourceId) -> { inputLevel, inputPeak, outputLevel, outputPeak }.
// Per-source INPUT level so a bound detector's silence gate reads ITS OWN device's
// signal (not the global/primary level — which would force-fail every hit on an
// extra device the user is actually playing). Output fields mirror the master and
// are 0 (monitoring is post-mix / engine-global). Bad id -> all zeros.
Napi::Value GetSourceLevels(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();
    SourceChain* s = (liveEngine && info.Length() >= 1 && info[0].IsNumber())
        ? getValidatedSource(liveEngine.get(), info, 0) : nullptr;
    obj.Set("inputLevel", s ? (double) s->getInputLevel() : 0.0);
    obj.Set("inputPeak",  s ? (double) s->getInputPeak()  : 0.0);
    obj.Set("outputLevel", 0.0);
    obj.Set("outputPeak", 0.0);
    return obj;
}

Napi::Value ResetPeaks(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->resetPeaks();
    return info.Env().Undefined();
}

// Backing-track mix bus RMS level — the engine's per-block running RMS after
// the backing volume fader but before the output-gain master. Returns 0.0 when
// the engine is unavailable or no backing track is loaded. Reads an atomic so
// it is safe to call from the JS thread without blocking the audio thread.
Napi::Value GetBackingLevel(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Number::New(info.Env(), liveEngine ? liveEngine->getBackingLevel() : 0.0f);
}

// ── MIDI ──────────────────────────────────────────────────────────────────────

Napi::Value SendMidiToSlot(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 4)
        return Napi::Boolean::New(env, false);

    // Typed + range-checked: unclamped channel/program used to trip JUCE
    // assertions (deep-read §2). Out-of-range now returns false cleanly.
    const auto slotId = slopsmith::addon::argSlotId(info, 0);
    const auto msgType = slopsmith::addon::argInt(info, 1, 0, 1);
    const auto channel = slopsmith::addon::argMidiChannel(info, 2);
    if (!slotId || !msgType || !channel)
        return Napi::Boolean::New(env, false);

    juce::MidiMessage midiMsg;
    if (*msgType == 0) // Program Change
    {
        const auto program = slopsmith::addon::argMidiByte(info, 3);
        if (!program) return Napi::Boolean::New(env, false);
        midiMsg = juce::MidiMessage::programChange(*channel, *program);
    }
    else // Control Change
    {
        const auto controller = slopsmith::addon::argMidiByte(info, 3);
        if (!controller) return Napi::Boolean::New(env, false);
        const auto value = slopsmith::addon::argMidiByte(info, 4);
        midiMsg = juce::MidiMessage::controllerEvent(*channel, *controller, value.value_or(0));
    }

    liveEngine->getSignalChain().queueMidiMessage(*slotId, midiMsg);
    return Napi::Boolean::New(env, true);
}

// ── Debug file logging ────────────────────────────────────────────────────────

// Redirect the C runtime's stderr stream to a file so the native
// [AudioEngine] / [audio-native] diagnostics are captured for a bug report on
// machines with no console (packaged Windows builds). Only invoked when
// SLOPSMITH_DEBUG is set. Returns "" on success, or an error description the
// JS layer logs as an [audio] line.
//
// freopen (not dup2): a packaged GUI-subsystem app has no console, so stderr
// has no valid fd — dup2 onto fileno(stderr) fails. freopen reassigns the
// stream itself and works with or without a console. freopen would close
// stderr before trying the path, so a bad path is ruled out FIRST with a
// throwaway fopen probe (which never touches stderr); only once the path is
// known-writable do we freopen. Append mode so the JS layer's header
// survives; unbuffered so a crash leaves a complete tail.
Napi::Value EnableFileLogging(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsString())
    {
        Napi::TypeError::New(env, "enableFileLogging(path) requires a string")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

#if defined(_WIN32)
    // Widen UTF-16 → wchar_t by value-converting each code unit (not a
    // reinterpret_cast — char16_t and wchar_t are distinct types even though
    // both are 16-bit on Windows). Wide path so a profile dir with non-ASCII
    // characters isn't mangled by the ANSI codepage (cf. src/vst-host/main.cpp,
    // which uses the GetEnvironmentVariableW / _wfopen wide path for the same
    // reason).
    const std::u16string u16 = info[0].As<Napi::String>().Utf16Value();
    const std::wstring wpath(u16.begin(), u16.end());
    FILE* probe = _wfopen(wpath.c_str(), L"a");
#else
    const std::string path = info[0].As<Napi::String>().Utf8Value();
    FILE* probe = std::fopen(path.c_str(), "a");
#endif
    if (probe == nullptr)
    {
        // Capture errno before Napi::String::New / std::to_string, which may
        // call library code that clobbers it.
        const int e = errno;
        return Napi::String::New(env, std::string("fopen failed (errno=")
                                      + std::to_string(e) + ")");
    }
    std::fclose(probe);  // path is writable; stderr never touched on this path

#if defined(_WIN32)
    FILE* fp = _wfreopen(wpath.c_str(), L"a", stderr);
#else
    FILE* fp = std::freopen(path.c_str(), "a", stderr);
#endif
    if (fp == nullptr)
    {
        const int e = errno;
        // freopen closes stderr before trying the path; on failure it's left
        // closed. The probe just verified the path, so this is near-impossible
        // — but redirect stderr to the null device so it's a valid sink rather
        // than a closed stream that could trip later fprintf(stderr) calls.
#if defined(_WIN32)
        std::freopen("NUL", "w", stderr);
#else
        std::freopen("/dev/null", "w", stderr);
#endif
        return Napi::String::New(env, std::string("freopen failed (errno=")
                                      + std::to_string(e) + ")");
    }

    // Unbuffered: each [AudioEngine] fprintf hits disk immediately, so a
    // crash mid-reconfigure still leaves the diagnostic line that explains it.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[audio-native] file logging enabled\n");
    return Napi::String::New(env, "");  // empty = success
}


} // namespace slopsmith::addon
