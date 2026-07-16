// Device enumeration/selection/control + stream sink bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
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

// ── Device Enumeration ────────────────────────────────────────────────────────

Napi::Value GetDeviceTypes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    // Device types are already scanned during init — safe to read from any thread
    auto types = liveEngine->getDeviceTypes();

    auto result = Napi::Array::New(env, types.size());

    for (int i = 0; i < types.size(); ++i)
    {
        auto obj = Napi::Object::New(env);
        obj.Set("name", types[i].name.toStdString());

        auto inputs = Napi::Array::New(env, types[i].inputDevices.size());
        for (int j = 0; j < types[i].inputDevices.size(); ++j)
            inputs.Set((uint32_t)j, types[i].inputDevices[j].toStdString());
        obj.Set("inputs", inputs);

        auto outputs = Napi::Array::New(env, types[i].outputDevices.size());
        for (int j = 0; j < types[i].outputDevices.size(); ++j)
            outputs.Set((uint32_t)j, types[i].outputDevices[j].toStdString());
        obj.Set("outputs", outputs);

        result.Set((uint32_t)i, obj);
    }

    return result;
}

Napi::Value GetSampleRates(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Array::New(env);

    auto rates = liveEngine->getSampleRates();
    auto result = Napi::Array::New(env, rates.size());
    for (int i = 0; i < rates.size(); ++i)
        result.Set((uint32_t)i, rates[i]);
    return result;
}

Napi::Value GetBufferSizes(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::Array::New(env);

    auto sizes = liveEngine->getBufferSizes();
    auto result = Napi::Array::New(env, sizes.size());
    for (int i = 0; i < sizes.size(); ++i)
        result.Set((uint32_t)i, sizes[i]);
    return result;
}

Napi::Value ProbeDeviceOptions(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto obj = Napi::Object::New(env);
    // 3-arg legacy (type, input, output) or 4-arg dual (inputType, input, outputType, output).
    auto arg0 = info.Length() > 0 && info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
    auto arg1 = info.Length() > 1 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "";
    auto arg2 = info.Length() > 2 && info[2].IsString() ? info[2].As<Napi::String>().Utf8Value() : "";
    auto arg3 = info.Length() > 3 && info[3].IsString() ? info[3].As<Napi::String>().Utf8Value() : "";

    std::string inputType = arg0;
    std::string inputName = arg1;
    std::string outputType;
    std::string outputName;
    if (info.Length() >= 4)
    {
        outputType = arg2;
        outputName = arg3;
    }
    else
    {
        outputType = arg0;
        outputName = arg2;
    }

    auto ratesArray = Napi::Array::New(env);
    auto buffersArray = Napi::Array::New(env);
    auto inputChannelsArray = Napi::Array::New(env);
    auto outputChannelsArray = Napi::Array::New(env);

    obj.Set("type", inputType);
    obj.Set("inputType", inputType);
    obj.Set("outputType", outputType);
    obj.Set("input", inputName);
    obj.Set("output", outputName);
    obj.Set("inputChannels", inputChannelsArray);
    obj.Set("outputChannels", outputChannelsArray);
    obj.Set("sampleRates", ratesArray);
    obj.Set("bufferSizes", buffersArray);
    obj.Set("compatible", true);
    if (!liveEngine)
    {
        obj.Set("error", "Audio engine not initialized");
        obj.Set("compatible", false);
        return obj;
    }

    // Probing constructs + destroys short-lived ASIO device objects
    // (DeviceSetup::probeDual / rateSupportedBy createDevice) — same
    // create/destroy-off-the-message-thread pattern as the lifecycle ops, so
    // it takes the same hop (PR #113 review finding 1). Query-only (never
    // open()), but a probe ASIOAudioIODevice owns the same reset-timer
    // machinery as the live one.
    auto optionsPtr = std::make_shared<AudioEngine::DeviceOptions>();
    if (!runDeviceLifecycleOp([liveEngine, inputType, inputName, outputType, outputName, optionsPtr] {
            *optionsPtr = liveEngine->probeDeviceOptionsDual(
                juce::String(inputType), juce::String(inputName),
                juce::String(outputType), juce::String(outputName));
        }))
    {
        obj.Set("error", "device probe did not complete (message thread unavailable or timed out)");
        obj.Set("compatible", false);
        return obj;
    }
    const auto& options = *optionsPtr;
    obj.Set("type", options.inputType.toStdString());      // legacy alias
    obj.Set("inputType", options.inputType.toStdString());
    obj.Set("outputType", options.outputType.toStdString());
    obj.Set("input", options.input.toStdString());
    obj.Set("output", options.output.toStdString());
    obj.Set("error", options.error.toStdString());
    obj.Set("compatible", options.compatible);

    inputChannelsArray = Napi::Array::New(env, options.inputChannels.size());
    for (int i = 0; i < options.inputChannels.size(); ++i)
        inputChannelsArray.Set((uint32_t)i, options.inputChannels[i].toStdString());
    obj.Set("inputChannels", inputChannelsArray);

    outputChannelsArray = Napi::Array::New(env, options.outputChannels.size());
    for (int i = 0; i < options.outputChannels.size(); ++i)
        outputChannelsArray.Set((uint32_t)i, options.outputChannels[i].toStdString());
    obj.Set("outputChannels", outputChannelsArray);

    ratesArray = Napi::Array::New(env, options.sampleRates.size());
    for (int i = 0; i < options.sampleRates.size(); ++i)
        ratesArray.Set((uint32_t)i, options.sampleRates[i]);
    obj.Set("sampleRates", ratesArray);

    buffersArray = Napi::Array::New(env, options.bufferSizes.size());
    for (int i = 0; i < options.bufferSizes.size(); ++i)
        buffersArray.Set((uint32_t)i, options.bufferSizes[i]);
    obj.Set("bufferSizes", buffersArray);

    return obj;
}

Napi::Value GetCurrentDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    auto obj = Napi::Object::New(env);
    const auto inputType = liveEngine->getCurrentInputDeviceType().toStdString();
    const auto outputType = liveEngine->getCurrentOutputDeviceType().toStdString();
    obj.Set("type", inputType);
    obj.Set("inputType", inputType);
    obj.Set("outputType", outputType);
    obj.Set("input", liveEngine->getCurrentInputDevice().toStdString());
    obj.Set("output", liveEngine->getCurrentOutputDevice().toStdString());
    obj.Set("sampleRate", liveEngine->getCurrentSampleRate());
    obj.Set("blockSize", liveEngine->getCurrentBlockSize());
    obj.Set("inputBlockSize", liveEngine->getCurrentInputBlockSize());
    obj.Set("outputBlockSize", liveEngine->getCurrentOutputBlockSize());
    obj.Set("latencyMs", liveEngine->getLatencyMs());
    obj.Set("duplex", liveEngine->isDuplex());
    return obj;
}

Napi::Value GetDeviceMetrics(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto obj = Napi::Object::New(env);
    if (!liveEngine)
    {
        obj.Set("duplex", true);
        obj.Set("inputOverflowCount", 0.0);
        obj.Set("outputUnderflowCount", 0.0);
        obj.Set("outputRingFillFrames", 0);
        obj.Set("outputRingCapacityFrames", 0);
        return obj;
    }
    const auto m = liveEngine->getDeviceMetrics();
    obj.Set("duplex", m.duplex);
    obj.Set("inputOverflowCount", static_cast<double>(m.inputOverflowCount));
    obj.Set("outputUnderflowCount", static_cast<double>(m.outputUnderflowCount));
    obj.Set("outputRingFillFrames", m.outputRingFillFrames);
    obj.Set("outputRingCapacityFrames", m.outputRingCapacityFrames);
    return obj;
}

// ── Device Selection ──────────────────────────────────────────────────────────

Napi::Value SetDeviceType(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsString())
        return Napi::Boolean::New(env, false);

    auto typeName = info[0].As<Napi::String>().Utf8Value();
    auto result = std::make_shared<bool>(false);
    if (!runDeviceLifecycleOp([liveEngine, typeName, result] {
            *result = liveEngine->setDeviceType(juce::String(typeName));
        }))
        return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, *result);
}

Napi::Value SetOutputDeviceType(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsString())
        return Napi::Boolean::New(env, false);
    auto typeName = info[0].As<Napi::String>().Utf8Value();
    auto result = std::make_shared<bool>(false);
    if (!runDeviceLifecycleOp([liveEngine, typeName, result] {
            *result = liveEngine->setOutputDeviceType(juce::String(typeName));
        }))
        return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, *result);
}

Napi::Value SetDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto result = Napi::Object::New(env);
    result.Set("ok", false);
    result.Set("duplex", true);
    result.Set("sampleRate", 0.0);
    result.Set("inputBlockSize", 0);
    result.Set("outputBlockSize", 0);
    result.Set("error", "");
    if (!liveEngine)
    {
        result.Set("error", "Audio engine not initialized");
        return result;
    }

    // Object payload: setDevice({inputType, inputDevice, outputType, outputDevice, sampleRate, bufferSize})
    // Legacy positional: setDevice(input, output, sampleRate, bufferSize)
    AudioEngine::DeviceConfig cfg;
    if (info.Length() > 0 && info[0].IsObject() && !info[0].IsNull() && !info[0].IsArray())
    {
        auto obj = info[0].As<Napi::Object>();
        auto readStr = [&](const char* key) -> std::string {
            if (obj.Has(key) && obj.Get(key).IsString()) return obj.Get(key).As<Napi::String>().Utf8Value();
            return {};
        };
        // Reject NaN/Infinity at the JS→C boundary so they can't poison
        // downstream comparisons (NaN <= 0 is false, so the validation
        // fallback in setAudioDevices() wouldn't catch them). Casting a
        // non-finite double to int is also UB in C++.
        auto readNum = [&](const char* key, double def) -> double {
            if (obj.Has(key) && obj.Get(key).IsNumber())
            {
                const double v = obj.Get(key).As<Napi::Number>().DoubleValue();
                if (std::isfinite(v)) return v;
            }
            return def;
        };
        cfg.inputType    = juce::String(readStr("inputType"));
        cfg.inputDevice  = juce::String(readStr("inputDevice"));
        if (cfg.inputDevice.isEmpty()) cfg.inputDevice = juce::String(readStr("input"));
        cfg.outputType   = juce::String(readStr("outputType"));
        cfg.outputDevice = juce::String(readStr("outputDevice"));
        if (cfg.outputDevice.isEmpty()) cfg.outputDevice = juce::String(readStr("output"));
        cfg.sampleRate = readNum("sampleRate", 48000.0);
        // Clamp before the double→int cast: finite-but-out-of-range values
        // (e.g. a JS-side bug passing 1e18) are UB to convert to int. readNum
        // already filtered non-finite; we just need a range check here.
        {
            const double bsd = readNum("bufferSize", 256.0);
            if (bsd >= 1.0 && bsd <= (double) (std::numeric_limits<int>::max) ())
                cfg.bufferSize = (int) bsd;
            else
                cfg.bufferSize = 256;
        }
    }
    else
    {
        auto input  = info.Length() > 0 && info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
        auto output = info.Length() > 1 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "";
        double sr = info.Length() > 2 && info[2].IsNumber() ? info[2].As<Napi::Number>().DoubleValue() : 48000.0;
        int bs    = info.Length() > 3 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 256;
        cfg.inputDevice = juce::String(input);
        cfg.outputDevice = juce::String(output);
        cfg.sampleRate = sr;
        cfg.bufferSize = bs;
    }

    // Main thread only — JUCE's ALSA backend deadlocks if called from a worker.
    // On Windows this hops to the JUCE message thread (runDeviceLifecycleOp)
    // so device destruction can't race the ASIO reset/device-change timers.
    auto res = std::make_shared<AudioEngine::DeviceConfigResult>();
    if (!runDeviceLifecycleOp([liveEngine, cfg, res] { *res = liveEngine->setAudioDevices(cfg); }))
    {
        result.Set("error", "device reconfigure did not complete (message thread unavailable or timed out)");
        return result;
    }
    const auto& r = *res;
    result.Set("ok", r.ok);
    result.Set("duplex", r.duplex);
    result.Set("sampleRate", r.sampleRate);
    result.Set("inputBlockSize", r.inputBlockSize);
    result.Set("outputBlockSize", r.outputBlockSize);
    result.Set("error", r.error.toStdString());
    return result;
}

// ── Audio Control ─────────────────────────────────────────────────────────────

Napi::Value StartAudio(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine())
        runDeviceLifecycleOp([liveEngine] { liveEngine->startAudio(); });
    return info.Env().Undefined();
}

Napi::Value StopAudio(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine())
        runDeviceLifecycleOp([liveEngine] { liveEngine->stopAudio(); });
    return info.Env().Undefined();
}

Napi::Value IsAudioRunning(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(info.Env(), liveEngine ? liveEngine->isAudioRunning() : false);
}

// ── Streamer mix output (PR1) ───────────────────────────────────────────────
// setStreamOutputDevice(typeName, deviceName) -> "" on success, else an error.
Napi::Value SetStreamOutputDevice(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return Napi::String::New(env, "no engine");
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString())
        return Napi::String::New(env, "setStreamOutputDevice(typeName:string, deviceName:string)");
    const std::string typeName = info[0].As<Napi::String>().Utf8Value();
    const std::string devName  = info[1].As<Napi::String>().Utf8Value();
    auto err = std::make_shared<juce::String>();
    if (!runDeviceLifecycleOp([liveEngine, typeName, devName, err] {
            *err = liveEngine->setStreamOutputDevice(juce::String(typeName), juce::String(devName));
        }))
        return Napi::String::New(env,
            "stream output open did not complete (message thread unavailable or timed out)");
    return Napi::String::New(env, err->toStdString());
}

// clearStreamOutput() -> undefined
Napi::Value ClearStreamOutput(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine)
        runDeviceLifecycleOp([liveEngine] { liveEngine->clearStreamOutput(); });
    return info.Env().Undefined();
}

// setStreamBus(includeBacking:boolean, includeGuitar:boolean, gain:number)
Napi::Value SetStreamBus(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 3 && info[0].IsBoolean() && info[1].IsBoolean() && info[2].IsNumber())
        liveEngine->setStreamBus(info[0].As<Napi::Boolean>().Value(),
                                 info[1].As<Napi::Boolean>().Value(),
                                 (float) info[2].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

// setStreamBusGain(gain:number)
Napi::Value SetStreamBusGain(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 1 && info[0].IsNumber())
        liveEngine->setStreamBusGain((float) info[0].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

// setRendererBus(enabled:boolean, gain:number)
Napi::Value SetRendererBus(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsBoolean() && info[1].IsNumber())
        liveEngine->setRendererBus(info[0].As<Napi::Boolean>().Value(),
                                   (float) info[1].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

// pushRendererAudio(interleavedLR:Float32Array, sourceRate:number) -> boolean
// Interleaved stereo (L0 R0 L1 R1 …); sourceRate is the renderer's
// AudioContext sample rate. Returns false when the bus is off / engine down /
// malformed args, so the renderer can stop pushing.
Napi::Value PushRendererAudio(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 2 || !info[0].IsTypedArray() || !info[1].IsNumber())
        return Napi::Boolean::New(env, false);
    auto ta = info[0].As<Napi::TypedArray>();
    if (ta.TypedArrayType() != napi_float32_array)
        return Napi::Boolean::New(env, false);
    auto f32 = info[0].As<Napi::Float32Array>();
    const size_t samples = f32.ElementLength();
    if (samples < 2)
        return Napi::Boolean::New(env, false);
    const int frames = (int) (samples / 2);
    const bool ok = liveEngine->pushRendererAudio(
        f32.Data(), frames, info[1].As<Napi::Number>().DoubleValue());
    return Napi::Boolean::New(env, ok);
}

// getRendererBusMetrics() -> {enabled, fillFrames, capacityFrames,
//                             pushedFrames, consumedFrames,
//                             underflowCount, overflowCount}
Napi::Value GetRendererBusMetrics(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    auto obj = Napi::Object::New(env);
    if (!liveEngine) return obj;
    const auto m = liveEngine->getRendererBusMetrics();
    obj.Set("enabled", m.enabled);
    obj.Set("fillFrames", m.fillFrames);
    obj.Set("capacityFrames", m.capacityFrames);
    obj.Set("pushedFrames", (double) m.pushedFrames);
    obj.Set("consumedFrames", (double) m.consumedFrames);
    obj.Set("underflowCount", (double) m.underflowCount);
    obj.Set("overflowCount", (double) m.overflowCount);
    return obj;
}

// getStreamSinkLevel() -> number (peak 0..1+)
Napi::Value GetStreamSinkLevel(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Number::New(info.Env(), liveEngine ? liveEngine->getStreamSinkLevel() : 0.0f);
}

// isStreamOutputActive() -> boolean
Napi::Value IsStreamOutputActive(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Boolean::New(info.Env(), liveEngine ? liveEngine->isStreamOutputActive() : false);
}

// getStreamUnderflowCount() -> number
Napi::Value GetStreamUnderflowCount(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Number::New(info.Env(),
        (double) (liveEngine ? liveEngine->getStreamUnderflowCount() : 0ull));
}

// getStreamOverflowCount() -> number (consumer fell a full ring behind; frames dropped)
Napi::Value GetStreamOverflowCount(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    return Napi::Number::New(info.Env(),
        (double) (liveEngine ? liveEngine->getStreamOverflowCount() : 0ull));
}

// setSourceInputChannel(sourceId, channel)
Napi::Value SetSourceInputChannel(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsNumber())
        if (SourceChain* s = getValidatedSource(liveEngine.get(), info, 0))
            s->setInputChannel(info[1].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

// setSourceVerifierOffset(sourceId, seconds) — per-source capture-latency
// correction the user dials in for an extra input device (the residual offset
// between that device's path and the primary's; not auto-measurable on JACK).
// Positive seconds DELAYS this source's scoring playhead, negative ADVANCES it.
Napi::Value SetSourceVerifierOffset(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsNumber())
    {
        const double sec = info[1].As<Napi::Number>().DoubleValue();
        if (std::isfinite(sec))
            if (SourceChain* s = getValidatedSource(liveEngine.get(), info, 0))
                s->setVerifierUserOffset(sec);
    }
    return info.Env().Undefined();
}

// setSourceMonitorMute(sourceId, mute)
Napi::Value SetSourceMonitorMute(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsBoolean())
        if (SourceChain* s = getValidatedSource(liveEngine.get(), info, 0))
            s->setMonitorMute(info[1].As<Napi::Boolean>().Value());
    return info.Env().Undefined();
}

// getSourceRawAudioFrame(sourceId, numSamples?) -> Float32Array
Napi::Value GetSourceRawAudioFrame(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsNumber())
        return Napi::Float32Array::New(env, 0);
    SourceChain* s = getValidatedSource(liveEngine.get(), info, 0);
    int numSamples = 4096;
    if (info.Length() > 1 && info[1].IsNumber())
        numSamples = info[1].As<Napi::Number>().Int32Value();
    if (!s || numSamples <= 0)
        return Napi::Float32Array::New(env, 0);
    auto frame = s->getRawAudioFrame(numSamples);
    auto out = Napi::Float32Array::New(env, frame.size());
    float* dst = out.Data();
    for (size_t i = 0; i < frame.size(); ++i)
        dst[i] = frame[i];
    return out;
}

// getSourcePitchDetection(sourceId) -> { frequency, confidence, midiNote, cents,
// noteName }. The no-detection shape when the id is bad/inactive.
Napi::Value GetSourcePitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();
    SourceChain* s = (liveEngine && info.Length() >= 1 && info[0].IsNumber())
        ? getValidatedSource(liveEngine.get(), info, 0) : nullptr;
    if (s)
    {
        auto det = s->getActiveDetection();
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

// getSourceRawPitchDetection(sourceId) -> raw YIN detection (bypasses ML), same
// shape as getSourcePitchDetection. Backs the per-source sustain glow / mono path.
Napi::Value GetSourceRawPitchDetection(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();
    SourceChain* s = (liveEngine && info.Length() >= 1 && info[0].IsNumber())
        ? getValidatedSource(liveEngine.get(), info, 0) : nullptr;
    if (s)
    {
        auto det = s->getRawPitchDetection();
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

// getSourceNoteVerdicts(sourceId, songTime?, playing?) -> verdict array, or null
// on a missing engine / bad id. Folds in the per-source playhead push like the
// legacy getNoteVerdicts.
Napi::Value GetSourceNoteVerdicts(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsNumber())
        return env.Null();
    SourceChain* s = getValidatedSource(liveEngine.get(), info, 0);
    if (!s) return env.Null();

    if (info.Length() >= 3 && info[1].IsNumber() && info[2].IsBoolean())
    {
        const double songTime = info[1].As<Napi::Number>().DoubleValue();
        if (std::isfinite(songTime))
            s->setPlayhead(songTime, info[2].As<Napi::Boolean>().Value());
    }

    const auto verdicts = s->getNoteVerdicts();
    auto arr = Napi::Array::New(env, verdicts.size());
    for (size_t i = 0; i < verdicts.size(); ++i)
    {
        const auto& v = verdicts[i];
        auto entry = Napi::Object::New(env);
        entry.Set("id", v.id);
        entry.Set("detected", v.detected);
        entry.Set("detectedSongTime", v.detectedSongTime);
        entry.Set("centsError", v.centsError);
        entry.Set("snr", v.snr);
        arr.Set((uint32_t) i, entry);
    }
    return arr;
}

// Push the song's note chart into the engine for continuous, background
// verification. The notedetect plugin calls this once per arrangement load;
// the engine's NoteVerifier thread then scores each note's timing window
// against the live playhead and input ring, so the renderer no longer runs a
// per-tick scoreChord IPC loop (which starved during dense passages).
//
// Expected payload:
// {
//   arrangement?: 'guitar'|'bass',  // default 'guitar'
//   stringCount?: number,           // default 6
//   tuningOffsets: number[],        // length should equal stringCount
//   capo?: number,                  // default 0
//   pitchCheckCents?: number,       // default 0 (energy-only)
//   harmonicSnr?: number,           // default 3.0
//   fundamentalRatio?: number,      // fundamental-presence gate, lower for
//                                   //  bass, <=0 disables (default 0.20)
//   timingTolerance?: number,       // seconds, default 0.1
//   notes: [{ id:string, t:number, s:number, f:number, sus:number,
//             ho?,po?,b?,sl?,hm?:boolean }, ...]
// }
// Returns true when the chart was accepted, false on a malformed payload or
// when no engine exists.
// Shared core: parse `reqObj` into a ChartUpdate and push it to `target`'s
// verifier. `target` is sources[0] for the legacy setChart and getSource(id) for
// the source-indexed setSourceChart. A malformed payload clears the target's
// chart (so a failed reload can't leave a stale chart scoring) and returns false.
Napi::Value setChartCore(Napi::Env env, Napi::Object reqObj, SourceChain* target)
{
    // Generous cap on the chart length — a full song's note list is well
    // under this, but it bounds the worst-case allocation a malformed payload
    // (claiming a gigantic JS array length) could force over IPC.
    static constexpr uint32_t kMaxChartNotes = 8192;

    // Rejecting a malformed chart must also drop whatever chart the verifier
    // currently holds — otherwise a failed (re)load leaves the previous
    // song's chart active and getNoteVerdicts() keeps emitting stale verdicts.
    auto reject = [&]() -> Napi::Value {
        if (target) target->clearChart();
        return Napi::Boolean::New(env, false);
    };

    NoteVerifier::ChartUpdate chart;
    if (reqObj.Has("arrangement") && reqObj.Get("arrangement").IsString())
        chart.arrangement = reqObj.Get("arrangement").As<Napi::String>().Utf8Value();
    if (reqObj.Has("stringCount") && reqObj.Get("stringCount").IsNumber())
        chart.stringCount = reqObj.Get("stringCount").As<Napi::Number>().Int32Value();
    if (reqObj.Has("capo") && reqObj.Get("capo").IsNumber())
        chart.capo = reqObj.Get("capo").As<Napi::Number>().Int32Value();
    if (reqObj.Has("pitchCheckCents") && reqObj.Get("pitchCheckCents").IsNumber())
        chart.pitchCheckCents = reqObj.Get("pitchCheckCents").As<Napi::Number>().FloatValue();
    if (reqObj.Has("harmonicSnr") && reqObj.Get("harmonicSnr").IsNumber())
        chart.harmonicSnr = reqObj.Get("harmonicSnr").As<Napi::Number>().FloatValue();
    if (reqObj.Has("fundamentalRatio") && reqObj.Get("fundamentalRatio").IsNumber())
    {
        // Drop NaN/Inf (see ScoreChord): a non-finite ratio poisons the
        // fundamental-presence gate; keep the safe 0.20 default.
        const float v = reqObj.Get("fundamentalRatio").As<Napi::Number>().FloatValue();
        if (std::isfinite(v)) chart.fundamentalRatio = v;
    }
    if (reqObj.Has("presenceRatio") && reqObj.Get("presenceRatio").IsNumber())
    {
        // Temporal-persistence floor, clamped to [0,1]. Saturate rather than
        // reject an out-of-range value: a stray >1 must NOT silently fall back to
        // 0 (legacy ever-present), which would reintroduce the false-accept this
        // guards against. Non-finite is ignored (keeps the 0 default).
        const float v = reqObj.Get("presenceRatio").As<Napi::Number>().FloatValue();
        if (std::isfinite(v)) chart.presenceRatio = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    if (reqObj.Has("timingTolerance") && reqObj.Get("timingTolerance").IsNumber())
        chart.timingTolerance = reqObj.Get("timingTolerance").As<Napi::Number>().DoubleValue();

    if (reqObj.Has("tuningOffsets") && reqObj.Get("tuningOffsets").IsArray())
    {
        auto arr = reqObj.Get("tuningOffsets").As<Napi::Array>();
        if (arr.Length() > 32) return reject();
        chart.tuningOffsets.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto v = arr.Get(i);
            if (!v.IsNumber()) return reject();
            chart.tuningOffsets.push_back(v.As<Napi::Number>().Int32Value());
        }
    }

    // ChordScorer requires exactly one tuning offset per string and otherwise
    // fails every note closed. Reject the chart here so a malformed payload
    // surfaces as setChart() == false rather than a silently all-miss session
    // the caller believes loaded fine.
    if ((int) chart.tuningOffsets.size() != chart.stringCount)
        return reject();

    Napi::Value notesVal = reqObj.Has("notes") ? reqObj.Get("notes") : env.Null();
    if (!notesVal.IsArray()) return reject();
    auto notesArr = notesVal.As<Napi::Array>();
    if (notesArr.Length() > kMaxChartNotes) return reject();

    chart.notes.reserve(notesArr.Length());
    for (uint32_t i = 0; i < notesArr.Length(); ++i)
    {
        auto v = notesArr.Get(i);
        if (!v.IsObject()) return reject();
        auto noteObj = v.As<Napi::Object>();

        // Every chart note must carry all five required fields with the right
        // type. Filling defaults for a missing field would push a bogus
        // time-0 note with an empty id — that breaks verdict-by-id alignment
        // — so reject the whole chart instead.
        const bool validNote =
            noteObj.Has("id")  && noteObj.Get("id").IsString() &&
            noteObj.Has("t")   && noteObj.Get("t").IsNumber() &&
            noteObj.Has("s")   && noteObj.Get("s").IsNumber() &&
            noteObj.Has("f")   && noteObj.Get("f").IsNumber() &&
            noteObj.Has("sus") && noteObj.Get("sus").IsNumber();
        if (!validNote) return reject();

        NoteVerifier::ChartNote n{};
        n.id = noteObj.Get("id").As<Napi::String>().Utf8Value();
        n.t = noteObj.Get("t").As<Napi::Number>().DoubleValue();
        n.string = noteObj.Get("s").As<Napi::Number>().Int32Value();
        n.fret = noteObj.Get("f").As<Napi::Number>().Int32Value();
        n.sus = noteObj.Get("sus").As<Napi::Number>().DoubleValue();
        auto truthy = [&noteObj](const char* key) {
            if (!noteObj.Has(key)) return false;
            return noteObj.Get(key).ToBoolean().Value();
        };
        n.ho = truthy("ho");
        n.po = truthy("po");
        n.b  = truthy("b");
        n.sl = truthy("sl");
        n.hm = truthy("hm");
        chart.notes.push_back(std::move(n));
    }

    target->setChart(chart);
    return Napi::Boolean::New(env, true);
}

// Legacy: setChart(chart) — targets sources[0]. Backward-compatible.
Napi::Value SetChart(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsObject())
    {
        if (liveEngine) liveEngine->clearChart();
        return Napi::Boolean::New(env, false);
    }
    return setChartCore(env, info[0].As<Napi::Object>(), liveEngine->getSource(0));
}

// Source-indexed: setSourceChart(sourceId, chart). Bad id / payload -> false.
Napi::Value SetSourceChart(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject())
        return Napi::Boolean::New(env, false);
    SourceChain* target = getValidatedSource(liveEngine.get(), info, 0);
    if (!target) return Napi::Boolean::New(env, false);
    return setChartCore(env, info[1].As<Napi::Object>(), target);
}

// Drain the verdicts the NoteVerifier thread has finalized since the last
// call. Returns an array of { id, detected, detectedSongTime, centsError, snr }.
//
// Optionally also pushes the renderer's playhead: getNoteVerdicts(songTime,
// playing). The plugin calls this once per detect tick, so folding the push in
// here advances the verifier's clock without a second IPC round-trip. A
// downlevel caller passing no args still just drains.
Napi::Value GetNoteVerdicts(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    // Null (not an empty array) on a missing engine — the bridge/preload
    // contract treats null as "unsupported/unavailable" so the renderer
    // feature-detects, matching detectNotes' no-engine path.
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return env.Null();

    // Push the playhead before draining so this tick's verdicts reflect it.
    // A JS NaN/Infinity passes IsNumber() — guard with isfinite so a bad
    // value can't corrupt the verifier's interpolated timing.
    if (info.Length() >= 2 && info[0].IsNumber() && info[1].IsBoolean())
    {
        const double songTime = info[0].As<Napi::Number>().DoubleValue();
        if (std::isfinite(songTime))
            liveEngine->setPlayhead(songTime, info[1].As<Napi::Boolean>().Value());
    }

    const auto verdicts = liveEngine->getNoteVerdicts();
    auto arr = Napi::Array::New(env, verdicts.size());
    for (size_t i = 0; i < verdicts.size(); ++i)
    {
        const auto& v = verdicts[i];
        auto entry = Napi::Object::New(env);
        entry.Set("id", v.id);
        entry.Set("detected", v.detected);
        entry.Set("detectedSongTime", v.detectedSongTime);
        entry.Set("centsError", v.centsError);
        entry.Set("snr", v.snr);
        arr.Set((uint32_t) i, entry);
    }
    return arr;
}

// Sample rate the audio device is running at. Notedetect's chord scorer
// needs this to map FFT bins to Hz; on the bridge path there's no
// AudioContext to read it from. Falls back to 48000 if the engine isn't
// ready (matches the historical fallback in screen.js) — and also if
// the engine is initialized but no device is currently active, which
// pins currentSampleRate to 0 internally and would otherwise propagate
// a divide-by-zero into the renderer's FFT-bin→Hz math.
Napi::Value GetSampleRate(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    constexpr double kFallbackSampleRate = 48000.0;
    auto liveEngine = snapshotEngine();
    if (!liveEngine)
        return Napi::Number::New(env, kFallbackSampleRate);
    const double sr = liveEngine->getCurrentSampleRate();
    if (!std::isfinite(sr) || sr <= 0.0)
        return Napi::Number::New(env, kFallbackSampleRate);
    return Napi::Number::New(env, sr);
}


Napi::Value GetLatencyBreakdown(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    auto liveEngine = snapshotEngine();
    if (!liveEngine) return obj;
    const auto b = liveEngine->getLatencyBreakdown();
    obj.Set("sampleRate", b.sampleRate);
    obj.Set("duplex", b.duplex);
    obj.Set("deviceBufferMs", b.deviceBufferMs);
    obj.Set("inputLatencyMs", b.inputLatencyMs);
    obj.Set("outputLatencyMs", b.outputLatencyMs);
    obj.Set("splitRingMs", b.splitRingMs);
    obj.Set("monitorTotalMs", b.monitorTotalMs);
    obj.Set("rendererBusMs", b.rendererBusMs);
    return obj;
}

} // namespace slopsmith::addon
