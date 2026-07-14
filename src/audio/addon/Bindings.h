#pragma once

// Binding declarations for the split N-API handler files (TLC phase 7b):
// DeviceBindings / ControlBindings / DetectionBindings / ChainBindings /
// BackingBindings. NodeAddon.cpp registers them in its export table.

#include <napi.h>

class AudioEngine;
class SourceChain;

namespace slopsmith::addon {

Napi::Value GetLatencyBreakdown(const Napi::CallbackInfo& info);

// Validate a JS source-id argument and return the live source (nullptr for
// missing / non-Number / non-finite / out-of-range). Shared by the
// source-indexed bindings across the split files.
SourceChain* getValidatedSource(AudioEngine* eng, const Napi::CallbackInfo& info, size_t argIndex);

Napi::Value AddSource(const Napi::CallbackInfo& info);
Napi::Value BindInputDevice(const Napi::CallbackInfo& info);
Napi::Value ClearChain(const Napi::CallbackInfo& info);
Napi::Value ClearStreamOutput(const Napi::CallbackInfo& info);
Napi::Value DetectNotes(const Napi::CallbackInfo& info);
Napi::Value EnableFileLogging(const Napi::CallbackInfo& info);
Napi::Value GetBackingDuration(const Napi::CallbackInfo& info);
Napi::Value GetBackingLevel(const Napi::CallbackInfo& info);
Napi::Value GetBackingPosition(const Napi::CallbackInfo& info);
Napi::Value GetBufferSizes(const Napi::CallbackInfo& info);
Napi::Value GetChainGeneration(const Napi::CallbackInfo& info);
Napi::Value GetChainState(const Napi::CallbackInfo& info);
Napi::Value GetCurrentDevice(const Napi::CallbackInfo& info);
Napi::Value GetDeviceMetrics(const Napi::CallbackInfo& info);
Napi::Value GetDeviceTypes(const Napi::CallbackInfo& info);
Napi::Value GetLevels(const Napi::CallbackInfo& info);
Napi::Value GetNoteVerdicts(const Napi::CallbackInfo& info);
Napi::Value GetParameters(const Napi::CallbackInfo& info);
Napi::Value GetPitchDetection(const Napi::CallbackInfo& info);
Napi::Value GetRawAudioFrame(const Napi::CallbackInfo& info);
Napi::Value GetRawPitchDetection(const Napi::CallbackInfo& info);
Napi::Value GetRendererBusMetrics(const Napi::CallbackInfo& info);
Napi::Value GetSampleRate(const Napi::CallbackInfo& info);
Napi::Value GetSampleRates(const Napi::CallbackInfo& info);
Napi::Value GetSourceLevels(const Napi::CallbackInfo& info);
Napi::Value GetSourceNoteVerdicts(const Napi::CallbackInfo& info);
Napi::Value GetSourcePitchDetection(const Napi::CallbackInfo& info);
Napi::Value GetSourceRawAudioFrame(const Napi::CallbackInfo& info);
Napi::Value GetSourceRawPitchDetection(const Napi::CallbackInfo& info);
Napi::Value GetStreamOverflowCount(const Napi::CallbackInfo& info);
Napi::Value GetStreamSinkLevel(const Napi::CallbackInfo& info);
Napi::Value GetStreamUnderflowCount(const Napi::CallbackInfo& info);
Napi::Value IsAudioRunning(const Napi::CallbackInfo& info);
Napi::Value IsBackingPlaying(const Napi::CallbackInfo& info);
Napi::Value IsMlNoteDetection(const Napi::CallbackInfo& info);
Napi::Value IsMonitorMuted(const Napi::CallbackInfo& info);
Napi::Value IsStreamOutputActive(const Napi::CallbackInfo& info);
Napi::Value ListInputDevices(const Napi::CallbackInfo& info);
Napi::Value ListSources(const Napi::CallbackInfo& info);
Napi::Value LoadBackingTrack(const Napi::CallbackInfo& info);
Napi::Value LoadNoteModel(const Napi::CallbackInfo& info);
Napi::Value MoveProcessor(const Napi::CallbackInfo& info);
Napi::Value ProbeDeviceOptions(const Napi::CallbackInfo& info);
Napi::Value PushRendererAudio(const Napi::CallbackInfo& info);
Napi::Value RemoveProcessor(const Napi::CallbackInfo& info);
Napi::Value RemoveSource(const Napi::CallbackInfo& info);
Napi::Value ResetPeaks(const Napi::CallbackInfo& info);
Napi::Value SavePreset(const Napi::CallbackInfo& info);
Napi::Value ScoreChord(const Napi::CallbackInfo& info);
Napi::Value ScoreSourceChord(const Napi::CallbackInfo& info);
Napi::Value SeekBacking(const Napi::CallbackInfo& info);
Napi::Value SendMidiToSlot(const Napi::CallbackInfo& info);
Napi::Value SetBackingSpeed(const Napi::CallbackInfo& info);
Napi::Value SetBranch(const Napi::CallbackInfo& info);
Napi::Value SetBranchSrc(const Napi::CallbackInfo& info);
Napi::Value SetBypass(const Napi::CallbackInfo& info);
Napi::Value SetChart(const Napi::CallbackInfo& info);
Napi::Value SetDevice(const Napi::CallbackInfo& info);
Napi::Value SetDeviceType(const Napi::CallbackInfo& info);
Napi::Value SetGain(const Napi::CallbackInfo& info);
Napi::Value SetInputChannel(const Napi::CallbackInfo& info);
Napi::Value AcquireMonitorMuteHold(const Napi::CallbackInfo& info);
Napi::Value ReleaseMonitorMuteHold(const Napi::CallbackInfo& info);
Napi::Value GetMonitorMuteState(const Napi::CallbackInfo& info);
Napi::Value SetMonitorKill(const Napi::CallbackInfo& info);
Napi::Value SetMonitorMute(const Napi::CallbackInfo& info);
Napi::Value SetMonitorMuteSuppressed(const Napi::CallbackInfo& info);
Napi::Value SetMultiBypass(const Napi::CallbackInfo& info);
Napi::Value SetNoiseGate(const Napi::CallbackInfo& info);
Napi::Value SetNoteDetectionEnabled(const Napi::CallbackInfo& info);
Napi::Value SetOutputDeviceType(const Napi::CallbackInfo& info);
Napi::Value SetPan(const Napi::CallbackInfo& info);
Napi::Value SetParameter(const Napi::CallbackInfo& info);
Napi::Value SetPostGain(const Napi::CallbackInfo& info);
Napi::Value SetRendererBus(const Napi::CallbackInfo& info);
Napi::Value MixerCreateChannel(const Napi::CallbackInfo& info);
Napi::Value MixerReleaseChannel(const Napi::CallbackInfo& info);
Napi::Value MixerPushChannel(const Napi::CallbackInfo& info);
Napi::Value MixerSetChannelGain(const Napi::CallbackInfo& info);
Napi::Value MixerSetChannelMute(const Napi::CallbackInfo& info);
Napi::Value MixerSetChannelGroup(const Napi::CallbackInfo& info);
Napi::Value MixerListChannels(const Napi::CallbackInfo& info);
Napi::Value SetSlotState(const Napi::CallbackInfo& info);
Napi::Value SetSourceChart(const Napi::CallbackInfo& info);
Napi::Value SetSourceInputChannel(const Napi::CallbackInfo& info);
Napi::Value SetSourceMonitorMute(const Napi::CallbackInfo& info);
Napi::Value SetSourceVerifierOffset(const Napi::CallbackInfo& info);
Napi::Value SetStreamBus(const Napi::CallbackInfo& info);
Napi::Value SetStreamBusGain(const Napi::CallbackInfo& info);
Napi::Value SetStreamOutputDevice(const Napi::CallbackInfo& info);
Napi::Value SetTonePolish(const Napi::CallbackInfo& info);
Napi::Value StartAudio(const Napi::CallbackInfo& info);
Napi::Value StartBacking(const Napi::CallbackInfo& info);
Napi::Value StopAudio(const Napi::CallbackInfo& info);
Napi::Value StopBacking(const Napi::CallbackInfo& info);
Napi::Value UnbindInputDevice(const Napi::CallbackInfo& info);
Napi::Value scoreChordCore(const Napi::CallbackInfo& info);
Napi::Value setChartCore(const Napi::CallbackInfo& info);

} // namespace slopsmith::addon
