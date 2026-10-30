#include "sonare_wrap_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/audio.h"
#include "engine/common.h"
#include "engine/realtime_engine.h"
#include "midi/live_push_args.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

bool ReadPositiveUint32(Napi::Env env, const Napi::Value& value, const char* label, uint32_t* out) {
  if (out == nullptr || !value.IsNumber()) {
    Napi::TypeError::New(env, std::string(label) + " must be a number")
        .ThrowAsJavaScriptException();
    return false;
  }
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number <= 0.0 || std::floor(number) != number ||
      number > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    Napi::RangeError::New(env, std::string(label) + " must be a positive uint32 integer")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<uint32_t>(number);
  return true;
}

bool ReadEngineSampleRate(Napi::Env env, const Napi::Value& value, double* out) {
  if (out == nullptr || !value.IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return false;
  }
  const double sample_rate = value.As<Napi::Number>().DoubleValue();
  if (!sonare::is_supported_sample_rate(sample_rate)) {
    Napi::RangeError::New(env, "sampleRate must be a whole number of hertz within 8000..384000")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = sample_rate;
  return true;
}

}  // namespace

Napi::Object RealtimeEngineWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "RealtimeEngine",
      {
          InstanceMethod<&RealtimeEngineWrap::Prepare>("prepare"),
          InstanceMethod<&RealtimeEngineWrap::Play>("play"),
          InstanceMethod<&RealtimeEngineWrap::Stop>("stop"),
          InstanceMethod<&RealtimeEngineWrap::SeekSample>("seekSample"),
          InstanceMethod<&RealtimeEngineWrap::SettleParameters>("settleParameters"),
          InstanceMethod<&RealtimeEngineWrap::FlushControlCommands>("flushControlCommands"),
          InstanceMethod<&RealtimeEngineWrap::SettleInsertParameters>("settleInsertParameters"),
          InstanceMethod<&RealtimeEngineWrap::ApplyCommandsDueNowPreservingFuture>(
              "applyCommandsDueNowPreservingFuture"),
          InstanceMethod<&RealtimeEngineWrap::SeekPpq>("seekPpq"),
          InstanceMethod<&RealtimeEngineWrap::SetTempo>("setTempo"),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignature>("setTimeSignature"),
          InstanceMethod<&RealtimeEngineWrap::ResetMasterLoudnessMeter>("resetMasterLoudnessMeter"),
          InstanceMethod<&RealtimeEngineWrap::PrimeOfflineParameters>("primeOfflineParameters"),
          InstanceMethod<&RealtimeEngineWrap::ResetProcessorState>("resetProcessorState"),
          InstanceMethod<&RealtimeEngineWrap::TailSamples>("tailSamples"),
          InstanceMethod<&RealtimeEngineWrap::GraphLatencySamplesQ8>("graphLatencySamplesQ8"),
          InstanceMethod<&RealtimeEngineWrap::SetTempoSegments>("setTempoSegments"),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignatureSegments>("setTimeSignatureSegments"),
          InstanceMethod<&RealtimeEngineWrap::SampleAtPpq>("sampleAtPpq"),
          InstanceMethod<&RealtimeEngineWrap::SetLoop>("setLoop"),
          InstanceMethod<&RealtimeEngineWrap::AddParameter>("addParameter"),
          InstanceMethod<&RealtimeEngineWrap::ParameterCount>("parameterCount"),
          InstanceMethod<&RealtimeEngineWrap::ParameterInfoByIndex>("parameterInfoByIndex"),
          InstanceMethod<&RealtimeEngineWrap::ParameterInfo>("parameterInfo"),
          InstanceMethod<&RealtimeEngineWrap::SetAutomationLane>("setAutomationLane"),
          InstanceMethod<&RealtimeEngineWrap::AutomationLaneCount>("automationLaneCount"),
          InstanceMethod<&RealtimeEngineWrap::SetMarkers>("setMarkers"),
          InstanceMethod<&RealtimeEngineWrap::MarkerCount>("markerCount"),
          InstanceMethod<&RealtimeEngineWrap::MarkerByIndex>("markerByIndex"),
          InstanceMethod<&RealtimeEngineWrap::Marker>("marker"),
          InstanceMethod<&RealtimeEngineWrap::SeekMarker>("seekMarker"),
          InstanceMethod<&RealtimeEngineWrap::SetLoopFromMarkers>("setLoopFromMarkers"),
          InstanceMethod<&RealtimeEngineWrap::SetMetronome>("setMetronome"),
          InstanceMethod<&RealtimeEngineWrap::Metronome>("metronome"),
          InstanceMethod<&RealtimeEngineWrap::CountInEndSample>("countInEndSample"),
          InstanceMethod<&RealtimeEngineWrap::SetClips>("setClips"),
          InstanceMethod<&RealtimeEngineWrap::UpsertClip>("upsertClip"),
          InstanceMethod<&RealtimeEngineWrap::RemoveClip>("removeClip"),
          InstanceMethod<&RealtimeEngineWrap::ClipCount>("clipCount"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackLanes>("setTrackLanes"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackSends>("setTrackSends"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackOutputBus>("setTrackOutputBus"),
          InstanceMethod<&RealtimeEngineWrap::SetLaneSidechain>("setLaneSidechain"),
          InstanceMethod<&RealtimeEngineWrap::ApplyProjectTimeline>("applyProjectTimeline"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackBuses>("setTrackBuses"),
          InstanceMethod<&RealtimeEngineWrap::SetBusSidechain>("setBusSidechain"),
          InstanceMethod<&RealtimeEngineWrap::SetMasterSidechain>("setMasterSidechain"),
          InstanceMethod<&RealtimeEngineWrap::CanSetLaneSidechain>("canSetLaneSidechain"),
          InstanceMethod<&RealtimeEngineWrap::CanSetBusSidechain>("canSetBusSidechain"),
          InstanceMethod<&RealtimeEngineWrap::CanSetMasterSidechain>("canSetMasterSidechain"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripJson>("setBusStripJson"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripEqBandJson>("setBusStripEqBandJson"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripJson>("setTrackStripJson"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripEqBandJson>("setTrackStripEqBandJson"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripInsertBypassed>(
              "setTrackStripInsertBypassed"),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripJson>("setMasterStripJson"),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripEqBandJson>("setMasterStripEqBandJson"),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripInsertBypassed>(
              "setMasterStripInsertBypassed"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripInsertParamByName>(
              "setTrackStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripInsertParamByName>(
              "setMasterStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripInsertParamByName>(
              "setBusStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::ApplyTrackStripInsertParamByNameNow>(
              "applyTrackStripInsertParamByNameNow"),
          InstanceMethod<&RealtimeEngineWrap::RestoreTrackStripInsertParamByName>(
              "restoreTrackStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::ApplyMasterStripInsertParamByNameNow>(
              "applyMasterStripInsertParamByNameNow"),
          InstanceMethod<&RealtimeEngineWrap::RestoreMasterStripInsertParamByName>(
              "restoreMasterStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::ApplyBusStripInsertParamByNameNow>(
              "applyBusStripInsertParamByNameNow"),
          InstanceMethod<&RealtimeEngineWrap::RestoreBusStripInsertParamByName>(
              "restoreBusStripInsertParamByName"),
          InstanceMethod<&RealtimeEngineWrap::ClearTrackInsertParameterBases>(
              "clearTrackInsertParameterBases"),
          InstanceMethod<&RealtimeEngineWrap::ClearMasterInsertParameterBases>(
              "clearMasterInsertParameterBases"),
          InstanceMethod<&RealtimeEngineWrap::ClearBusInsertParameterBases>(
              "clearBusInsertParameterBases"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripInsertBypassed>(
              "setBusStripInsertBypassed"),
          InstanceMethod<&RealtimeEngineWrap::ResolveTrackInsertAutomationId>(
              "resolveTrackInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveTrackLaneAutomationId>(
              "resolveTrackLaneAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveBusAutomationId>("resolveBusAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveMasterAutomationId>(
              "resolveMasterAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveMasterInsertAutomationId>(
              "resolveMasterInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveBusInsertAutomationId>(
              "resolveBusInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::InsertParameterConstructedValue>(
              "insertParameterConstructedValue"),
          InstanceMethod<&RealtimeEngineWrap::ResolveInstrumentAutomationId>(
              "resolveInstrumentAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPan>("setTrackStripPan"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanLaw>("setTrackStripPanLaw"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanMode>("setTrackStripPanMode"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripDualPan>("setTrackStripDualPan"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripSurroundPan>("setTrackStripSurroundPan"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPan>("setBusStripPan"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPanLaw>("setBusStripPanLaw"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPanMode>("setBusStripPanMode"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripDualPan>("setBusStripDualPan"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripChannelDelaySamples>(
              "setTrackStripChannelDelaySamples"),
          InstanceMethod<&RealtimeEngineWrap::CreateClipPageProvider>("createClipPageProvider"),
          InstanceMethod<&RealtimeEngineWrap::SupplyClipPage>("supplyClipPage"),
          InstanceMethod<&RealtimeEngineWrap::ClearClipPage>("clearClipPage"),
          InstanceMethod<&RealtimeEngineWrap::DestroyClipPageProvider>("destroyClipPageProvider"),
          InstanceMethod<&RealtimeEngineWrap::PopClipPageRequest>("popClipPageRequest"),
          InstanceMethod<&RealtimeEngineWrap::SetClipPagePrefetchFrames>(
              "setClipPagePrefetchFrames"),
          InstanceMethod<&RealtimeEngineWrap::ClipPagePrefetchFrames>("clipPagePrefetchFrames"),
          InstanceMethod<&RealtimeEngineWrap::SetCaptureBuffer>("setCaptureBuffer"),
          InstanceMethod<&RealtimeEngineWrap::ArmCapture>("armCapture"),
          InstanceMethod<&RealtimeEngineWrap::SetCapturePunch>("setCapturePunch"),
          InstanceMethod<&RealtimeEngineWrap::SetCaptureSource>("setCaptureSource"),
          InstanceMethod<&RealtimeEngineWrap::SetRecordOffsetSamples>("setRecordOffsetSamples"),
          InstanceMethod<&RealtimeEngineWrap::SetInputMonitor>("setInputMonitor"),
          InstanceMethod<&RealtimeEngineWrap::ResetCapture>("resetCapture"),
          InstanceMethod<&RealtimeEngineWrap::CaptureStatus>("captureStatus"),
          InstanceMethod<&RealtimeEngineWrap::CapturedAudio>("capturedAudio"),
          InstanceMethod<&RealtimeEngineWrap::SetGraph>("setGraph"),
          InstanceMethod<&RealtimeEngineWrap::GraphNodeCount>("graphNodeCount"),
          InstanceMethod<&RealtimeEngineWrap::GraphConnectionCount>("graphConnectionCount"),
          InstanceMethod<&RealtimeEngineWrap::Process>("process"),
          InstanceMethod<&RealtimeEngineWrap::ProcessWithMonitor>("processWithMonitor"),
          InstanceMethod<&RealtimeEngineWrap::RenderOffline>("renderOffline"),
          InstanceMethod<&RealtimeEngineWrap::FinishOfflineRender>("finishOfflineRender"),
          InstanceMethod<&RealtimeEngineWrap::BounceOffline>("bounceOffline"),
          InstanceMethod<&RealtimeEngineWrap::FreezeOffline>("freezeOffline"),
          InstanceMethod<&RealtimeEngineWrap::DrainTelemetry>("drainTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::DrainMeterTelemetry>("drainMeterTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::DrainMeterTelemetryWide>("drainMeterTelemetryWide"),
          InstanceMethod<&RealtimeEngineWrap::MeterTargetInsertGainReduction>(
              "meterTargetInsertGainReduction"),
          InstanceMethod<&RealtimeEngineWrap::ConfigureScopeTelemetry>("configureScopeTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::DrainScopeTelemetry>("drainScopeTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::SetParameter>("setParameter"),
          InstanceMethod<&RealtimeEngineWrap::SetParameterSmoothed>("setParameterSmoothed"),
          InstanceMethod<&RealtimeEngineWrap::SetParamSmoothingMs>("setParamSmoothingMs"),
          InstanceMethod<&RealtimeEngineWrap::SetSoloMute>("setSoloMute"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackMonitorMode>("setTrackMonitorMode"),
          InstanceMethod<&RealtimeEngineWrap::ClearParameters>("clearParameters"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiClips>("setMidiClips"),
          InstanceMethod<&RealtimeEngineWrap::SetBuiltinInstrument>("setBuiltinInstrument"),
          InstanceMethod<&RealtimeEngineWrap::SetSynthInstrument>("setSynthInstrument"),
          InstanceMethod<&RealtimeEngineWrap::LoadSoundFont>("loadSoundFont"),
          InstanceMethod<&RealtimeEngineWrap::SetSf2Instrument>("setSf2Instrument"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInstrument>("clearMidiInstrument"),
          InstanceMethod<&RealtimeEngineWrap::MidiInstrumentCount>("midiInstrumentCount"),
          InstanceMethod<&RealtimeEngineWrap::BindMidiCc>("bindMidiCc"),
          InstanceMethod<&RealtimeEngineWrap::BindMidiCcBinding>("bindMidiCcBinding"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiCcBindings>("clearMidiCcBindings"),
          InstanceMethod<&RealtimeEngineWrap::MidiCcBindingCount>("midiCcBindingCount"),
          InstanceMethod<&RealtimeEngineWrap::SetControllerProfile>("setControllerProfile"),
          InstanceMethod<&RealtimeEngineWrap::BindController>("bindController"),
          InstanceMethod<&RealtimeEngineWrap::ClearControllerBindings>("clearControllerBindings"),
          InstanceMethod<&RealtimeEngineWrap::ControllerBindingCount>("controllerBindingCount"),
          InstanceMethod<&RealtimeEngineWrap::SetControllerVelocityMeaningful>(
              "setControllerVelocityMeaningful"),
          InstanceMethod<&RealtimeEngineWrap::ControllerVelocityMeaningful>(
              "controllerVelocityMeaningful"),
          InstanceMethod<&RealtimeEngineWrap::SetControllerNoteTracking>(
              "setControllerNoteTracking"),
          InstanceMethod<&RealtimeEngineWrap::ControllerNoteTracking>("controllerNoteTracking"),
          InstanceMethod<&RealtimeEngineWrap::SetArticulation>("setArticulation"),
          InstanceMethod<&RealtimeEngineWrap::Articulation>("articulation"),
          InstanceMethod<&RealtimeEngineWrap::SetPartRig>("setPartRig"),
          InstanceMethod<&RealtimeEngineWrap::LegatoFallbackCount>("legatoFallbackCount"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiFx>("setMidiFx"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiFx>("clearMidiFx"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiInputSource>("setMidiInputSource"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInputSource>("clearMidiInputSource"),
          InstanceMethod<&RealtimeEngineWrap::MidiInputPendingCount>("midiInputPendingCount"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOn>("pushMidiInputNoteOn"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOff>("pushMidiInputNoteOff"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputCc>("pushMidiInputCc"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputPitchBend>("pushMidiInputPitchBend"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputChannelPressure>(
              "pushMidiInputChannelPressure"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputPolyPressure>(
              "pushMidiInputPolyPressure"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOn>("pushMidiNoteOn"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOff>("pushMidiNoteOff"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiCc>("pushMidiCc"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPitchBend>("pushMidiPitchBend"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiChannelPressure>("pushMidiChannelPressure"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPolyPressure>("pushMidiPolyPressure"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPanic>("pushMidiPanic"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiSysex>("pushMidiSysex"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiUmp>("pushMidiUmp"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputUmp>("pushMidiInputUmp"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiProgram>("pushMidiProgram"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputProgram>("pushMidiInputProgram"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiDestinationExternal>(
              "setMidiDestinationExternal"),
          InstanceMethod<&RealtimeEngineWrap::SetExternalMidiClockEnabled>(
              "setExternalMidiClockEnabled"),
          InstanceMethod<&RealtimeEngineWrap::ExternalMidiDroppedCount>("externalMidiDroppedCount"),
          InstanceMethod<&RealtimeEngineWrap::ClipPageRequestOverflowCount>(
              "clipPageRequestOverflowCount"),
          InstanceMethod<&RealtimeEngineWrap::WarpStretchOverflowCount>("warpStretchOverflowCount"),
          InstanceMethod<&RealtimeEngineWrap::SetWarpVoiceCapacity>("setWarpVoiceCapacity"),
          InstanceMethod<&RealtimeEngineWrap::WarpVoiceCapacity>("warpVoiceCapacity"),
          InstanceMethod<&RealtimeEngineWrap::DrainExternalMidi>("drainExternalMidi"),
          InstanceMethod<&RealtimeEngineWrap::GetTransportState>("getTransportState"),
          InstanceMethod<&RealtimeEngineWrap::Destroy>("destroy"),
      });
  exports.Set("RealtimeEngine", func);
  return exports;
}

RealtimeEngineWrap::RealtimeEngineWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RealtimeEngineWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t abi_version = sonare_engine_abi_version();
  if (abi_version != kExpectedEngineAbiVersion) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_ABI_MISMATCH,
        "libsonare engine ABI mismatch: native binary reports version " +
            std::to_string(abi_version) + ", expected " +
            std::to_string(kExpectedEngineAbiVersion) +
            ". The prebuilt addon is incompatible with this binding.");
    return;
  }
  double sample_rate = 48000.0;
  if (info.Length() > 0 && !info[0].IsUndefined() &&
      !ReadEngineSampleRate(env, info[0], &sample_rate)) {
    return;
  }
  int max_block_size = 128;
  int64_t command_capacity = 1024;
  int64_t telemetry_capacity = 1024;
  int max_channels = 64;
  if (!OptionalIntArg(env, info, 1, "maxBlockSize", 128, &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 1024, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 1024, &telemetry_capacity) ||
      !OptionalIntArg(env, info, 4, "maxChannels", 64, &max_channels)) {
    return;
  }

  SonareError err = sonare_engine_create(&engine_);
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return;
  err = sonare_engine_prepare_with_channels(engine_, sample_rate, max_block_size,
                                            static_cast<size_t>(command_capacity),
                                            static_cast<size_t>(telemetry_capacity), max_channels);
  ThrowIfError(env, err);
  SONARE_NODE_CATCH_VOID(env)
}

RealtimeEngineWrap::~RealtimeEngineWrap() { ReleaseNativeResources(); }

void RealtimeEngineWrap::ReleaseNativeResources() {
  if (engine_ != nullptr) {
    sonare_engine_destroy(engine_);
    engine_ = nullptr;
  }
  // After the engine is gone nothing can still page from a provider, so the
  // providers are released next. Entries are erased by destroyClipPageProvider,
  // which is what makes a second pass here a no-op rather than a double free.
  for (const auto& entry : clip_page_providers_) {
    if (entry.second != nullptr) sonare_clip_page_provider_destroy(entry.second);
  }
  clip_page_providers_.clear();
  // A capture buffer sized for a long session is the largest allocation the
  // wrap owns, and once the engine is gone nothing can read it: capturedAudio()
  // rejects a destroyed engine. Release it here rather than waiting for the JS
  // object to be collected.
  capture_buffers_.clear();
  capture_buffers_.shrink_to_fit();
  capture_ptrs_.clear();
  capture_ptrs_.shrink_to_fit();
  capture_capacity_frames_ = 0;
}

Napi::Value RealtimeEngineWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  // Both leading arguments are required: an absent one reads as undefined and is
  // rejected by the reader below, so it reaches the caller as a TypeError rather
  // than as a silent no-op.
  double sample_rate = 0.0;
  if (!ReadEngineSampleRate(env, info[0], &sample_rate)) return env.Undefined();
  int max_block_size = 0;
  int64_t command_capacity = 1024;
  int64_t telemetry_capacity = 1024;
  int max_channels = 64;
  if (!RequiredIntArg(env, info, 1, "maxBlockSize", &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 1024, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 1024, &telemetry_capacity) ||
      !OptionalIntArg(env, info, 4, "maxChannels", 64, &max_channels)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_prepare_with_channels(
                        engine_, sample_rate, max_block_size, static_cast<size_t>(command_capacity),
                        static_cast<size_t>(telemetry_capacity), max_channels));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Play(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_play(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Stop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_stop(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t sample = 0;
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "timelineSample", 0, &sample) ||
      !OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_seek_sample(engine_, sample, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SettleParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_settle_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::FlushControlCommands(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_flush_control_commands(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SettleInsertParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_settle_insert_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyCommandsDueNowPreservingFuture(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_apply_commands_due_now_preserving_future(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double ppq = 0.0;
  int64_t deadline = -1;
  if (!RequiredDoubleArg(env, info, 0, "ppq", &ppq) ||
      !OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_seek_ppq(engine_, ppq, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double bpm = 0.0;
  if (!RequiredDoubleArg(env, info, 0, "bpm", &bpm)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_tempo(engine_, bpm));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignature(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int numerator = 0;
  int denominator = 0;
  if (!RequiredIntArg(env, info, 0, "numerator", &numerator) ||
      !RequiredIntArg(env, info, 1, "denominator", &denominator)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_time_signature(engine_, numerator, denominator));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResetMasterLoudnessMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t render_frame = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &render_frame)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_reset_master_loudness_meter(engine_, render_frame));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PrimeOfflineParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int num_channels = 0;
  int block_size = 0;
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !RequiredIntArg(env, info, 1, "blockSize", &block_size)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_prime_offline_parameters(engine_, num_channels, block_size));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResetProcessorState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t render_frame = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &render_frame)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_reset_processor_state(engine_, render_frame));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::TailSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int tail = 0;
  ThrowIfError(env, sonare_engine_tail_samples(engine_, &tail));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(tail));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::GraphLatencySamplesQ8(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int latency_q8 = 0;
  ThrowIfError(env, sonare_engine_graph_latency_samples_q8(engine_, &latency_q8));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(latency_q8));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempoSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "segments", &input)) return env.Undefined();
  std::vector<SonareProjectTempoSegment> segments;
  segments.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "tempo segment must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareProjectTempoSegment segment{};
    if (!RequiredDoubleProperty(env, obj, "startPpq", &segment.start_ppq)) {
      return env.Undefined();
    }
    if (!RequiredDoubleProperty(env, obj, "bpm", &segment.bpm)) return env.Undefined();
    segment.start_sample = 0.0;
    segment.end_bpm = DoubleProperty(obj, "endBpm", 0.0);
    if (env.IsExceptionPending()) return env.Undefined();
    segments.push_back(segment);
  }
  ThrowIfError(env, sonare_engine_set_tempo_segments(engine_, segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignatureSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  Napi::Array input;
  if (!RequiredArrayValue(env, info[0], "segments", &input)) return env.Undefined();
  std::vector<SonareProjectTimeSignatureSegment> segments;
  segments.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "time signature segment must be an object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareProjectTimeSignatureSegment segment{};
    if (!RequiredDoubleProperty(env, obj, "startPpq", &segment.start_ppq)) {
      return env.Undefined();
    }
    if (!RequiredIntProperty(env, obj, "numerator", &segment.numerator)) return env.Undefined();
    if (!RequiredIntProperty(env, obj, "denominator", &segment.denominator)) {
      return env.Undefined();
    }
    segments.push_back(segment);
  }
  ThrowIfError(
      env, sonare_engine_set_time_signature_segments(engine_, segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SampleAtPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double ppq = 0.0;
  if (!RequiredDoubleArg(env, info, 0, "ppq", &ppq)) return env.Undefined();
  int64_t sample = 0;
  ThrowIfError(env, sonare_engine_sample_at_ppq(engine_, ppq, &sample));
  return Napi::Number::New(env, static_cast<double>(sample));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  bool enabled = true;
  if (!RequiredDoubleArg(env, info, 0, "startPpq", &start_ppq) ||
      !RequiredDoubleArg(env, info, 1, "endPpq", &end_ppq) ||
      !OptionalBoolArg(env, info, 2, "enabled", true, &enabled)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_loop(engine_, start_ppq, end_ppq, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::AddParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  SonareParameterInfo parameter{};
  if (!ReadParameter(info, 0, &parameter)) return env.Undefined();
  ThrowIfError(env, sonare_engine_add_parameter(engine_, &parameter));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ParameterCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_parameter_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ParameterInfoByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareParameterInfo parameter{};
  ThrowIfError(env, sonare_engine_parameter_info_by_index(engine_, index, &parameter));
  if (env.IsExceptionPending()) return env.Undefined();
  return ParameterToObject(env, parameter);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ParameterInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t id = 0;
  if (!RequiredUint32Arg(env, info, 0, "index", &id)) return env.Undefined();
  SonareParameterInfo parameter{};
  ThrowIfError(env, sonare_engine_parameter_info(engine_, id, &parameter));
  if (env.IsExceptionPending()) return env.Undefined();
  return ParameterToObject(env, parameter);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() <= 1 || !info[1].IsArray()) {
    Napi::TypeError::New(env, "expected an array of automation points")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t param_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "paramId", 0, &param_id)) return env.Undefined();
  Napi::Array input = info[1].As<Napi::Array>();
  std::vector<SonareAutomationPoint> points;
  points.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "automation point must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareAutomationPoint point{};
    if (!RequiredDoubleProperty(env, obj, "ppq", &point.ppq)) return env.Undefined();
    const Napi::Value point_value = obj.Get("value");
    node_require_property_type(env, point_value.IsNumber(), "value", "a number");
    point.value = node_narrow_finite_float(env, point_value, "value");
    point.curve_to_next = IntProperty(obj, "curveToNext", 0);
    if (env.IsExceptionPending()) return env.Undefined();
    points.push_back(point);
  }
  ThrowIfError(env,
               sonare_engine_set_automation_lane(engine_, param_id, points.data(), points.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::AutomationLaneCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_automation_lane_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of markers").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<SonareEngineMarker> markers;
  markers.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "marker must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareEngineMarker marker{};
    if (!ReadPositiveUint32(env, obj.Get("id"), "marker id", &marker.id)) {
      return env.Undefined();
    }
    marker.kind = sonare_node::MidiByteProperty(env, obj, "kind", SONARE_MARKER_KIND_MARKER);
    marker.key_fifths = sonare_node::Int8Property(obj, "keyFifths", 0);
    marker.key_minor = BoolProperty(obj, "keyMinor", false) ? 1 : 0;
    if (!RequiredDoubleProperty(env, obj, "ppq", &marker.ppq)) return env.Undefined();
    CopyString(marker.name, sizeof(marker.name), StringProperty(obj, "name", ""));
    if (env.IsExceptionPending()) return env.Undefined();
    markers.push_back(marker);
  }
  ThrowIfError(env, sonare_engine_set_markers(engine_, markers.data(), markers.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MarkerCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t count = 0;
  ThrowIfError(env, sonare_engine_marker_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MarkerByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareEngineMarker marker{};
  ThrowIfError(env, sonare_engine_marker_by_index(engine_, index, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  return MarkerToObject(env, marker);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Marker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t id = 0;
  if (!RequiredUint32Arg(env, info, 0, "id", &id)) return env.Undefined();
  SonareEngineMarker marker{};
  ThrowIfError(env, sonare_engine_marker(engine_, id, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  return MarkerToObject(env, marker);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekMarker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t id = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "markerId", &id) ||
      !OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_seek_marker(engine_, id, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLoopFromMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t start_id = 0;
  uint32_t end_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "startMarkerId", &start_id) ||
      !RequiredUint32Arg(env, info, 1, "endMarkerId", &end_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_loop_from_markers(engine_, start_id, end_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMetronome(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a metronome config object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineMetronomeConfig config{};
  config.enabled = BoolProperty(obj, "enabled", false) ? 1 : 0;
  config.beat_gain = FloatProperty(obj, "beatGain", 0.35f);
  config.accent_gain = FloatProperty(obj, "accentGain", 0.7f);
  config.click_samples = IntProperty(obj, "clickSamples", kZeroIsSentinel);
  config.click_seconds = DoubleProperty(obj, "clickSeconds", 0.0);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_metronome(engine_, &config));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Metronome(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  SonareEngineMetronomeConfig config{};
  ThrowIfError(env, sonare_engine_metronome(engine_, &config));
  if (env.IsExceptionPending()) return env.Undefined();
  return MetronomeToObject(env, config);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CountInEndSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int64_t start_sample = 0;
  int bars = 0;
  if (!RequiredInt64Arg(env, info, 0, "startSample", &start_sample) ||
      !RequiredIntArg(env, info, 1, "bars", &bars)) {
    return env.Undefined();
  }
  int64_t out = 0;
  ThrowIfError(env, sonare_engine_count_in_end_sample(engine_, start_sample, bars, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t param_id = 0;
  float value = 0.0f;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "paramId", &param_id) ||
      !RequiredFloatArg(env, info, 1, "value", &value) ||
      !OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_parameter(engine_, param_id, value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::InsertParameterConstructedValue(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t param_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "paramId", &param_id)) return env.Undefined();
  float value = 0.0f;
  ThrowIfError(env, sonare_engine_insert_parameter_constructed_value(engine_, param_id, &value));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, value);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParameterSmoothed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t param_id = 0;
  float value = 0.0f;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "paramId", &param_id) ||
      !RequiredFloatArg(env, info, 1, "value", &value) ||
      !OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_parameter_smoothed(engine_, param_id, value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParamSmoothingMs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  float smoothing_ms = 0.0f;
  if (!RequiredFloatArg(env, info, 0, "smoothingMs", &smoothing_ms)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_param_smoothing_ms(engine_, smoothing_ms));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetSoloMute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t lane_index = 0;
  bool solo = false;
  bool mute = false;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "laneIndex", &lane_index) ||
      !RequiredBoolArg(env, info, 1, "solo", &solo) ||
      !RequiredBoolArg(env, info, 2, "mute", &mute) ||
      !OptionalInt64Arg(env, info, 3, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_set_solo_mute(engine_, lane_index, solo ? 1 : 0, mute ? 1 : 0, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackMonitorMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t lane_index = 0;
  int mode_value = 0;
  int64_t deadline = -1;
  if (!RequiredUint32Arg(env, info, 0, "laneIndex", &lane_index) ||
      !RequiredIntArg(env, info, 1, "mode", &mode_value) ||
      !OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  const auto mode = static_cast<SonareEngineTrackMonitorMode>(mode_value);
  ThrowIfError(env, sonare_engine_set_track_monitor_mode(engine_, lane_index, mode, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  ThrowIfError(env, sonare_engine_clear_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClipPageRequestOverflowCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_clip_page_request_overflow_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::WarpStretchOverflowCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_warp_stretch_overflow_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetWarpVoiceCapacity(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t voices = 0;
  if (!RequiredUint32Arg(env, info, 0, "voices", &voices)) return env.Undefined();
  if (voices > SONARE_ENGINE_MAX_WARP_VOICES) {
    Napi::RangeError::New(env, "voices must be an integer in [0, " +
                                   std::to_string(SONARE_ENGINE_MAX_WARP_VOICES) + "]")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_warp_voice_capacity(engine_, voices));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::WarpVoiceCapacity(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  uint32_t voices = 0;
  ThrowIfError(env, sonare_engine_warp_voice_capacity(engine_, &voices));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(voices));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::GetTransportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  SonareTransportState state{};
  ThrowIfError(env, sonare_engine_get_transport_state(engine_, &state));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  const Napi::Boolean playing = Napi::Boolean::New(env, state.playing != 0);
  out.Set("playing", playing);
  out.Set("isPlaying", playing);
  out.Set("looping", Napi::Boolean::New(env, state.looping != 0));
  out.Set("renderFrame", Napi::Number::New(env, static_cast<double>(state.render_frame)));
  out.Set("samplePosition", Napi::Number::New(env, static_cast<double>(state.sample_position)));
  out.Set("ppq", Napi::Number::New(env, state.ppq_position));
  out.Set("bpm", Napi::Number::New(env, state.bpm));
  out.Set("loopStartPpq", Napi::Number::New(env, state.loop_start_ppq));
  out.Set("loopEndPpq", Napi::Number::New(env, state.loop_end_ppq));
  out.Set("sampleRate", Napi::Number::New(env, state.sample_rate));
  out.Set("barStartPpq", Napi::Number::New(env, state.bar_start_ppq));
  out.Set("barCount", Napi::Number::New(env, static_cast<double>(state.bar_count)));
  Napi::Object time_signature = Napi::Object::New(env);
  time_signature.Set("numerator", Napi::Number::New(env, state.time_signature.numerator));
  time_signature.Set("denominator", Napi::Number::New(env, state.time_signature.denominator));
  time_signature.Set("confidence",
                     Napi::Number::New(env, static_cast<double>(state.time_signature.confidence)));
  out.Set("timeSignature", time_signature);
  out.Set("beat", Napi::Number::New(env, static_cast<double>(state.beat)));
  out.Set("beatFraction", Napi::Number::New(env, state.beat_fraction));
  return out;
  SONARE_NODE_CATCH(env)
}

void RealtimeEngineWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY(void) info;
  ReleaseNativeResources();
  SONARE_NODE_CATCH_VOID(env)
}
