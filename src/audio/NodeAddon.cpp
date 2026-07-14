// Slopsmith Audio Engine — Node.js Native Addon (N-API)
// Bridges the JUCE-based C++ audio engine to Electron via node-addon-api.
// All audio processing happens in C++; JS communicates via IPC.

#include <napi.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "AudioEngine.h"
#include "VSTHost.h"

#include "VSTTrace.h"
#include "NAMProcessor.h"
#include "IRLoader.h"
#include "Sandbox/SandboxedProcessor.h"
#include "Sandbox/CrashAttribution.h"

#include <juce_events/juce_events.h>

#include "addon/AddonContext.h"
#include "addon/NapiHelpers.h"
#include "addon/ChainOps.h"
#include "addon/EditorWindows.h"
#include "addon/Bindings.h"

using slopsmith::addon::closeAllPluginEditorWindows;
using slopsmith::addon::destroyAllPluginEditorWindowsOnMessageThread;
using slopsmith::addon::OpenPluginEditor;
using slopsmith::addon::ClosePluginEditor;
using slopsmith::addon::decodeStateBlob;
using slopsmith::addon::LoadVST;
using slopsmith::addon::LoadNAMModel;
using slopsmith::addon::LoadIR;
using slopsmith::addon::ReplaceIR;
using slopsmith::addon::LoadPreset;
using slopsmith::addon::AddSource;
using slopsmith::addon::BindInputDevice;
using slopsmith::addon::ClearChain;
using slopsmith::addon::ClearStreamOutput;
using slopsmith::addon::DetectNotes;
using slopsmith::addon::EnableFileLogging;
using slopsmith::addon::GetBackingDuration;
using slopsmith::addon::GetBackingLevel;
using slopsmith::addon::GetBackingPosition;
using slopsmith::addon::GetBufferSizes;
using slopsmith::addon::GetChainGeneration;
using slopsmith::addon::GetChainState;
using slopsmith::addon::GetCurrentDevice;
using slopsmith::addon::GetDeviceMetrics;
using slopsmith::addon::GetDeviceTypes;
using slopsmith::addon::GetLevels;
using slopsmith::addon::GetNoteVerdicts;
using slopsmith::addon::GetParameters;
using slopsmith::addon::GetPitchDetection;
using slopsmith::addon::GetRawAudioFrame;
using slopsmith::addon::GetRawPitchDetection;
using slopsmith::addon::GetRendererBusMetrics;
using slopsmith::addon::GetSampleRate;
using slopsmith::addon::GetSampleRates;
using slopsmith::addon::GetSourceLevels;
using slopsmith::addon::GetSourceNoteVerdicts;
using slopsmith::addon::GetSourcePitchDetection;
using slopsmith::addon::GetSourceRawAudioFrame;
using slopsmith::addon::GetSourceRawPitchDetection;
using slopsmith::addon::GetStreamOverflowCount;
using slopsmith::addon::GetStreamSinkLevel;
using slopsmith::addon::GetStreamUnderflowCount;
using slopsmith::addon::IsAudioRunning;
using slopsmith::addon::IsBackingPlaying;
using slopsmith::addon::IsMlNoteDetection;
using slopsmith::addon::IsMonitorMuted;
using slopsmith::addon::IsStreamOutputActive;
using slopsmith::addon::ListInputDevices;
using slopsmith::addon::ListSources;
using slopsmith::addon::LoadBackingTrack;
using slopsmith::addon::LoadNoteModel;
using slopsmith::addon::MoveProcessor;
using slopsmith::addon::ProbeDeviceOptions;
using slopsmith::addon::PushRendererAudio;
using slopsmith::addon::RemoveProcessor;
using slopsmith::addon::RemoveSource;
using slopsmith::addon::ResetPeaks;
using slopsmith::addon::SavePreset;
using slopsmith::addon::ScoreChord;
using slopsmith::addon::ScoreSourceChord;
using slopsmith::addon::SeekBacking;
using slopsmith::addon::SendMidiToSlot;
using slopsmith::addon::SetBackingSpeed;
using slopsmith::addon::SetBranch;
using slopsmith::addon::SetBranchSrc;
using slopsmith::addon::SetBypass;
using slopsmith::addon::SetChart;
using slopsmith::addon::SetDevice;
using slopsmith::addon::SetDeviceType;
using slopsmith::addon::SetGain;
using slopsmith::addon::SetInputChannel;
using slopsmith::addon::SetMonitorKill;
using slopsmith::addon::SetMonitorMute;
using slopsmith::addon::SetMonitorMuteSuppressed;
using slopsmith::addon::AcquireMonitorMuteHold;
using slopsmith::addon::ReleaseMonitorMuteHold;
using slopsmith::addon::GetMonitorMuteState;
using slopsmith::addon::GetLatencyBreakdown;
using slopsmith::addon::SetMultiBypass;
using slopsmith::addon::SetNoiseGate;
using slopsmith::addon::SetNoteDetectionEnabled;
using slopsmith::addon::SetOutputDeviceType;
using slopsmith::addon::SetPan;
using slopsmith::addon::SetParameter;
using slopsmith::addon::SetPostGain;
using slopsmith::addon::SetRendererBus;
using slopsmith::addon::MixerCreateChannel;
using slopsmith::addon::MixerReleaseChannel;
using slopsmith::addon::MixerPushChannel;
using slopsmith::addon::MixerSetChannelGain;
using slopsmith::addon::MixerSetChannelMute;
using slopsmith::addon::MixerSetChannelGroup;
using slopsmith::addon::MixerListChannels;
using slopsmith::addon::SetSlotState;
using slopsmith::addon::SetSourceChart;
using slopsmith::addon::SetSourceInputChannel;
using slopsmith::addon::SetSourceMonitorMute;
using slopsmith::addon::SetSourceVerifierOffset;
using slopsmith::addon::SetStreamBus;
using slopsmith::addon::SetStreamBusGain;
using slopsmith::addon::SetStreamOutputDevice;
using slopsmith::addon::SetTonePolish;
using slopsmith::addon::StartAudio;
using slopsmith::addon::StartBacking;
using slopsmith::addon::StopAudio;
using slopsmith::addon::StopBacking;
using slopsmith::addon::UnbindInputDevice;
using slopsmith::addon::scoreChordCore;
using slopsmith::addon::setChartCore;

// Lifetime/threading moved to addon/AddonContext (TLC phase 6); the usings
// keep the 100+ existing binding bodies unchanged.
using slopsmith::addon::snapshotEngine;
using slopsmith::addon::snapshotVstHost;
using slopsmith::addon::dispatchOnMessageThread;
using slopsmith::addon::registerPendingLoad;
using slopsmith::addon::unregisterPendingLoad;
using slopsmith::addon::cancelAllPendingLoads;
using slopsmith::addon::doShutdown;





// Destroys every in-process plugin editor window. MUST be called on the message
// thread — lives in addon/EditorWindows now (TLC phase 7).

// ── Lifecycle ─────────────────────────────────────────────────────────────────

static Napi::Value Init(const Napi::CallbackInfo& info)
{
    // Engine/vstHost creation + message-thread start live on AddonContext;
    // the UI teardown hook runs at shutdown BEFORE engine.reset() (#56).
    slopsmith::addon::initialize([] { destroyAllPluginEditorWindowsOnMessageThread(); });
    return info.Env().Undefined();
}

static Napi::Value Shutdown(const Napi::CallbackInfo& info)
{
    doShutdown();
    return info.Env().Undefined();
}

// ── VST Plugin Scanning ──────────────────────────────────────────────────────

class ScanPluginsWorker : public Napi::AsyncWorker
{
public:
    ScanPluginsWorker(Napi::Env env, Napi::Promise::Deferred deferred, juce::StringArray dirs)
        : Napi::AsyncWorker(env), deferred(deferred), directories(std::move(dirs)) {}

    void Execute() override
    {
        auto host = snapshotVstHost();
        if (!host) return;
        host->scanDirectories(directories, [](float, const juce::String&) {});
    }

    void OnOK() override
    {
        auto env = Env();
        auto result = Napi::Array::New(env);

        if (auto host = snapshotVstHost())
        {
            auto plugins = host->getKnownPlugins();
            for (int i = 0; i < plugins.size(); ++i)
            {
                auto obj = Napi::Object::New(env);
                obj.Set("name", plugins[i].name.toStdString());
                obj.Set("manufacturer", plugins[i].manufacturer.toStdString());
                obj.Set("category", plugins[i].category.toStdString());
                obj.Set("format", plugins[i].formatName.toStdString());
                obj.Set("path", plugins[i].fileOrIdentifier.toStdString());
                obj.Set("uid", plugins[i].uid.toStdString());
                obj.Set("isInstrument", plugins[i].isInstrument);
                result.Set((uint32_t)i, obj);
            }
        }

        deferred.Resolve(result);
    }

    void OnError(const Napi::Error& error) override
    {
        deferred.Reject(error.Value());
    }

private:
    Napi::Promise::Deferred deferred;
    juce::StringArray directories;
};

static Napi::Value ScanPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    juce::StringArray dirs;
    if (info.Length() > 0 && info[0].IsArray())
    {
        auto arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i)
            dirs.add(juce::String(arr.Get(i).As<Napi::String>().Utf8Value()));
    }
    else
    {
        dirs = VSTHost::getDefaultScanDirectories();
    }

    auto worker = new ScanPluginsWorker(env, deferred, dirs);
    worker->Queue();
    return deferred.Promise();
}

static Napi::Value GetKnownPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto result = Napi::Array::New(env);

    if (auto host = snapshotVstHost())
    {
        auto plugins = host->getKnownPlugins();
        for (int i = 0; i < plugins.size(); ++i)
        {
            auto obj = Napi::Object::New(env);
            obj.Set("name", plugins[i].name.toStdString());
            obj.Set("manufacturer", plugins[i].manufacturer.toStdString());
            obj.Set("category", plugins[i].category.toStdString());
            obj.Set("format", plugins[i].formatName.toStdString());
            obj.Set("path", plugins[i].fileOrIdentifier.toStdString());
            obj.Set("uid", plugins[i].uid.toStdString());
            obj.Set("isInstrument", plugins[i].isInstrument);
            result.Set((uint32_t)i, obj);
        }
    }

    return result;
}

static Napi::Value SavePluginList(const Napi::CallbackInfo& info)
{
    if (info.Length() == 0) return info.Env().Undefined();
    if (auto host = snapshotVstHost())
        host->savePluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

static Napi::Value LoadPluginList(const Napi::CallbackInfo& info)
{
    if (info.Length() == 0) return info.Env().Undefined();
    if (auto host = snapshotVstHost())
        host->loadPluginList(juce::File(juce::String(info[0].As<Napi::String>().Utf8Value())));
    return info.Env().Undefined();
}

// Register the plugins the renderer's VST crash guard recorded as having
// crashed the app on a previous run. shouldSandbox() then routes them through
// the out-of-process sandbox. Expects a single array-of-strings argument.
static Napi::Value SetCrashedPlugins(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    juce::StringArray paths;
    if (info.Length() > 0 && info[0].IsArray())
    {
        auto arr = info[0].As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i)
        {
            auto value = arr.Get(i);
            if (value.IsString())
                paths.add(juce::String(value.As<Napi::String>().Utf8Value()));
        }
    }
    slopsmith::sandbox::setCrashedPlugins(paths);
    return env.Undefined();
}

// Arm the native last-chance crash attributor with the path to the crash-guard
// sentinel file (src/main/vst-crash-guard.ts owns it). A fatal in-process fault
// inside a loaded .vst3 then stamps the sentinel before the process dies, so the
// next launch sandboxes the offender — covering crashes that arrive outside the
// JS load/editor sentinel windows (e.g. a plugin WndProc on WM_ACTIVATEAPP).
// No-op on non-Windows. See issue #35.
static Napi::Value SetVstCrashSentinelPath(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (info.Length() > 0 && info[0].IsString())
        slopsmith::sandbox::installVstCrashAttribution(
            juce::String(info[0].As<Napi::String>().Utf8Value()));
    return env.Undefined();
}

// ── Plugin Editor Window ──────────────────────────────────────────────────────

// ── Module Registration ───────────────────────────────────────────────────────

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
    // Lifecycle
    exports.Set("init", Napi::Function::New(env, Init));
    exports.Set("shutdown", Napi::Function::New(env, Shutdown));
    exports.Set("enableFileLogging", Napi::Function::New(env, EnableFileLogging));

    // Devices
    exports.Set("getDeviceTypes", Napi::Function::New(env, GetDeviceTypes));
    exports.Set("getSampleRates", Napi::Function::New(env, GetSampleRates));
    exports.Set("getBufferSizes", Napi::Function::New(env, GetBufferSizes));
    exports.Set("probeDeviceOptions", Napi::Function::New(env, ProbeDeviceOptions));
    exports.Set("getCurrentDevice", Napi::Function::New(env, GetCurrentDevice));
    exports.Set("setDeviceType", Napi::Function::New(env, SetDeviceType));
    exports.Set("setInputDeviceType", Napi::Function::New(env, SetDeviceType));
    exports.Set("setOutputDeviceType", Napi::Function::New(env, SetOutputDeviceType));
    exports.Set("setDevice", Napi::Function::New(env, SetDevice));
    exports.Set("getDeviceMetrics", Napi::Function::New(env, GetDeviceMetrics));

    // Audio control
    exports.Set("startAudio", Napi::Function::New(env, StartAudio));
    exports.Set("stopAudio", Napi::Function::New(env, StopAudio));
    exports.Set("isAudioRunning", Napi::Function::New(env, IsAudioRunning));

    // Gain
    exports.Set("setGain", Napi::Function::New(env, SetGain));
    exports.Set("setInputChannel", Napi::Function::New(env, SetInputChannel));
    exports.Set("setMonitorMute", Napi::Function::New(env, SetMonitorMute));
    exports.Set("setMonitorMuteSuppressed", Napi::Function::New(env, SetMonitorMuteSuppressed));
    exports.Set("acquireMonitorMuteHold", Napi::Function::New(env, AcquireMonitorMuteHold));
    exports.Set("releaseMonitorMuteHold", Napi::Function::New(env, ReleaseMonitorMuteHold));
    exports.Set("getMonitorMuteState", Napi::Function::New(env, GetMonitorMuteState));
    exports.Set("getLatencyBreakdown", Napi::Function::New(env, GetLatencyBreakdown));
    exports.Set("isMonitorMuted", Napi::Function::New(env, IsMonitorMuted));
    exports.Set("setMonitorKill", Napi::Function::New(env, SetMonitorKill));
    exports.Set("setNoiseGate", Napi::Function::New(env, SetNoiseGate));
    exports.Set("setTonePolish", Napi::Function::New(env, SetTonePolish));

    // Metering
    exports.Set("getLevels", Napi::Function::New(env, GetLevels));
    exports.Set("getSourceLevels", Napi::Function::New(env, GetSourceLevels));
    exports.Set("resetPeaks", Napi::Function::New(env, ResetPeaks));
    exports.Set("getBackingLevel", Napi::Function::New(env, GetBackingLevel));

    // Pitch detection
    exports.Set("getPitchDetection", Napi::Function::New(env, GetPitchDetection));
    exports.Set("getRawPitchDetection", Napi::Function::New(env, GetRawPitchDetection));
    exports.Set("getRawAudioFrame", Napi::Function::New(env, GetRawAudioFrame));
    exports.Set("scoreChord", Napi::Function::New(env, ScoreChord));
    exports.Set("setChart", Napi::Function::New(env, SetChart));
    exports.Set("getNoteVerdicts", Napi::Function::New(env, GetNoteVerdicts));

    // Multi-input source-indexed API. The un-suffixed methods above keep
    // targeting source 0 for backward compatibility.
    exports.Set("addSource", Napi::Function::New(env, AddSource));
    exports.Set("removeSource", Napi::Function::New(env, RemoveSource));
    exports.Set("listSources", Napi::Function::New(env, ListSources));
    exports.Set("listInputDevices", Napi::Function::New(env, ListInputDevices));
    exports.Set("bindInputDevice", Napi::Function::New(env, BindInputDevice));
    exports.Set("unbindInputDevice", Napi::Function::New(env, UnbindInputDevice));
    exports.Set("setStreamOutputDevice", Napi::Function::New(env, SetStreamOutputDevice));
    exports.Set("clearStreamOutput", Napi::Function::New(env, ClearStreamOutput));
    exports.Set("setStreamBus", Napi::Function::New(env, SetStreamBus));
    exports.Set("setStreamBusGain", Napi::Function::New(env, SetStreamBusGain));
    exports.Set("setRendererBus", Napi::Function::New(env, SetRendererBus));
    exports.Set("pushRendererAudio", Napi::Function::New(env, PushRendererAudio));
    exports.Set("getRendererBusMetrics", Napi::Function::New(env, GetRendererBusMetrics));
    exports.Set("mixerCreateChannel", Napi::Function::New(env, MixerCreateChannel));
    exports.Set("mixerReleaseChannel", Napi::Function::New(env, MixerReleaseChannel));
    exports.Set("mixerPushChannel", Napi::Function::New(env, MixerPushChannel));
    exports.Set("mixerSetChannelGain", Napi::Function::New(env, MixerSetChannelGain));
    exports.Set("mixerSetChannelMute", Napi::Function::New(env, MixerSetChannelMute));
    exports.Set("mixerSetChannelGroup", Napi::Function::New(env, MixerSetChannelGroup));
    exports.Set("mixerListChannels", Napi::Function::New(env, MixerListChannels));
    exports.Set("getStreamSinkLevel", Napi::Function::New(env, GetStreamSinkLevel));
    exports.Set("isStreamOutputActive", Napi::Function::New(env, IsStreamOutputActive));
    exports.Set("getStreamUnderflowCount", Napi::Function::New(env, GetStreamUnderflowCount));
    exports.Set("getStreamOverflowCount", Napi::Function::New(env, GetStreamOverflowCount));
    exports.Set("setSourceInputChannel", Napi::Function::New(env, SetSourceInputChannel));
    exports.Set("setSourceVerifierOffset", Napi::Function::New(env, SetSourceVerifierOffset));
    exports.Set("setSourceMonitorMute", Napi::Function::New(env, SetSourceMonitorMute));
    exports.Set("setSourceChart", Napi::Function::New(env, SetSourceChart));
    exports.Set("scoreSourceChord", Napi::Function::New(env, ScoreSourceChord));
    exports.Set("getSourceNoteVerdicts", Napi::Function::New(env, GetSourceNoteVerdicts));
    exports.Set("getSourceRawAudioFrame", Napi::Function::New(env, GetSourceRawAudioFrame));
    exports.Set("getSourcePitchDetection", Napi::Function::New(env, GetSourcePitchDetection));
    exports.Set("getSourceRawPitchDetection", Napi::Function::New(env, GetSourceRawPitchDetection));
    exports.Set("getSampleRate", Napi::Function::New(env, GetSampleRate));
    exports.Set("loadNoteModel", Napi::Function::New(env, LoadNoteModel));
    exports.Set("isMlNoteDetection", Napi::Function::New(env, IsMlNoteDetection));
    exports.Set("setNoteDetectionEnabled", Napi::Function::New(env, SetNoteDetectionEnabled));
    exports.Set("detectNotes", Napi::Function::New(env, DetectNotes));

    // VST scanning
    exports.Set("scanPlugins", Napi::Function::New(env, ScanPlugins));
    exports.Set("getKnownPlugins", Napi::Function::New(env, GetKnownPlugins));
    exports.Set("savePluginList", Napi::Function::New(env, SavePluginList));
    exports.Set("loadPluginList", Napi::Function::New(env, LoadPluginList));
    exports.Set("setCrashedPlugins", Napi::Function::New(env, SetCrashedPlugins));
    exports.Set("setVstCrashSentinelPath", Napi::Function::New(env, SetVstCrashSentinelPath));

    // Signal chain
    exports.Set("loadVST", Napi::Function::New(env, LoadVST));
    exports.Set("loadNAMModel", Napi::Function::New(env, LoadNAMModel));
    exports.Set("loadIR", Napi::Function::New(env, LoadIR));
    exports.Set("replaceIR", Napi::Function::New(env, ReplaceIR));
    exports.Set("removeProcessor", Napi::Function::New(env, RemoveProcessor));
    exports.Set("moveProcessor", Napi::Function::New(env, MoveProcessor));
    exports.Set("setBypass", Napi::Function::New(env, SetBypass));
    exports.Set("setPan", Napi::Function::New(env, SetPan));
    exports.Set("setBranch", Napi::Function::New(env, SetBranch));
    exports.Set("setPostGain", Napi::Function::New(env, SetPostGain));
    exports.Set("setBranchSrc", Napi::Function::New(env, SetBranchSrc));
    exports.Set("clearChain", Napi::Function::New(env, ClearChain));
    exports.Set("getChainState", Napi::Function::New(env, GetChainState));
    exports.Set("getChainGeneration", Napi::Function::New(env, GetChainGeneration));
    exports.Set("openPluginEditor", Napi::Function::New(env, OpenPluginEditor));
    exports.Set("closePluginEditor", Napi::Function::New(env, ClosePluginEditor));

    // Parameters
    exports.Set("getParameters", Napi::Function::New(env, GetParameters));
    exports.Set("setParameter", Napi::Function::New(env, SetParameter));
    exports.Set("setSlotState", Napi::Function::New(env, SetSlotState));

    // MIDI
    exports.Set("sendMidiToSlot", Napi::Function::New(env, SendMidiToSlot));

    // Backing track
    exports.Set("loadBackingTrack", Napi::Function::New(env, LoadBackingTrack));
    exports.Set("startBacking", Napi::Function::New(env, StartBacking));
    exports.Set("stopBacking", Napi::Function::New(env, StopBacking));
    exports.Set("seekBacking", Napi::Function::New(env, SeekBacking));
    exports.Set("getBackingPosition", Napi::Function::New(env, GetBackingPosition));
    exports.Set("getBackingDuration", Napi::Function::New(env, GetBackingDuration));
    exports.Set("isBackingPlaying", Napi::Function::New(env, IsBackingPlaying));
    exports.Set("setBackingSpeed", Napi::Function::New(env, SetBackingSpeed));

    // Presets
    exports.Set("savePreset", Napi::Function::New(env, SavePreset));
    exports.Set("loadPreset", Napi::Function::New(env, LoadPreset));
    exports.Set("setMultiBypass", Napi::Function::New(env, SetMultiBypass));

    // Drain JUCE message thread + sandbox subprocesses before DLL unload, so a
    // JS process exit without an explicit addon.shutdown() doesn't crash in
    // static destructors.
    napi_add_env_cleanup_hook(env, [](void*) { doShutdown(); }, nullptr);

    return exports;
}

NODE_API_MODULE(slopsmith_audio, InitModule)
