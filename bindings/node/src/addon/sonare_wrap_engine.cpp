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
    Napi::RangeError::New(env, std::string(label) + " must be an integer in [1, 4294967295]")
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
          InstanceMethod<&RealtimeEngineWrap::Prepare>("prepare", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Play>("play", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Stop>("stop", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SeekSample>("seekSample", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SettleParameters>("settleParameters",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::FlushControlCommands>("flushControlCommands",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SettleInsertParameters>("settleInsertParameters",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ApplyCommandsDueNowPreservingFuture>(
              "applyCommandsDueNowPreservingFuture", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SeekPpq>("seekPpq", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTempo>("setTempo", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignature>("setTimeSignature",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResetMasterLoudnessMeter>("resetMasterLoudnessMeter",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PrimeOfflineParameters>("primeOfflineParameters",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResetProcessorState>("resetProcessorState",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::TailSamples>("tailSamples", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MaxBlockSize>("maxBlockSize", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::GraphLatencySamplesQ8>("graphLatencySamplesQ8",
                                                                     napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTempoSegments>("setTempoSegments",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignatureSegments>("setTimeSignatureSegments",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SampleAtPpq>("sampleAtPpq", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetLoop>("setLoop", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::AddParameter>("addParameter", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ParameterCount>("parameterCount",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ParameterInfoByIndex>("parameterInfoByIndex",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ParameterInfo>("parameterInfo", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetAutomationLane>("setAutomationLane",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::AutomationLaneCount>("automationLaneCount",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMarkers>("setMarkers", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MarkerCount>("markerCount", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MarkerByIndex>("markerByIndex", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Marker>("marker", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SeekMarker>("seekMarker", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetLoopFromMarkers>("setLoopFromMarkers",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMetronome>("setMetronome", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Metronome>("metronome", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CountInEndSample>("countInEndSample",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetClips>("setClips", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::UpsertClip>("upsertClip", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::RemoveClip>("removeClip", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClipCount>("clipCount", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackLanes>("setTrackLanes", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackSends>("setTrackSends", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackOutputBus>("setTrackOutputBus",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetLaneSidechain>("setLaneSidechain",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ApplyProjectTimeline>("applyProjectTimeline",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackBuses>("setTrackBuses", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusSidechain>("setBusSidechain",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMasterSidechain>("setMasterSidechain",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CanSetLaneSidechain>("canSetLaneSidechain",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CanSetBusSidechain>("canSetBusSidechain",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CanSetMasterSidechain>("canSetMasterSidechain",
                                                                     napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripJson>("setBusStripJson",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripEqBandJson>("setBusStripEqBandJson",
                                                                     napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripJson>("setTrackStripJson",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripEqBandJson>("setTrackStripEqBandJson",
                                                                       napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripInsertBypassed>(
              "setTrackStripInsertBypassed", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripJson>("setMasterStripJson",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripEqBandJson>("setMasterStripEqBandJson",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripInsertBypassed>(
              "setMasterStripInsertBypassed", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripInsertParamByName>(
              "setTrackStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMasterStripInsertParamByName>(
              "setMasterStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripInsertParamByName>(
              "setBusStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ApplyTrackStripInsertParamByNameNow>(
              "applyTrackStripInsertParamByNameNow", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::RestoreTrackStripInsertParamByName>(
              "restoreTrackStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ApplyMasterStripInsertParamByNameNow>(
              "applyMasterStripInsertParamByNameNow", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::RestoreMasterStripInsertParamByName>(
              "restoreMasterStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ApplyBusStripInsertParamByNameNow>(
              "applyBusStripInsertParamByNameNow", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::RestoreBusStripInsertParamByName>(
              "restoreBusStripInsertParamByName", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearTrackInsertParameterBases>(
              "clearTrackInsertParameterBases", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearMasterInsertParameterBases>(
              "clearMasterInsertParameterBases", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearBusInsertParameterBases>(
              "clearBusInsertParameterBases", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripInsertBypassed>(
              "setBusStripInsertBypassed", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveTrackInsertAutomationId>(
              "resolveTrackInsertAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveTrackLaneAutomationId>(
              "resolveTrackLaneAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveBusAutomationId>("resolveBusAutomationId",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveMasterAutomationId>(
              "resolveMasterAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveMasterInsertAutomationId>(
              "resolveMasterInsertAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveBusInsertAutomationId>(
              "resolveBusInsertAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::InsertParameterConstructedValue>(
              "insertParameterConstructedValue", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResolveInstrumentAutomationId>(
              "resolveInstrumentAutomationId", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPan>("setTrackStripPan",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanLaw>("setTrackStripPanLaw",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanMode>("setTrackStripPanMode",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripDualPan>("setTrackStripDualPan",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripSurroundPan>("setTrackStripSurroundPan",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPan>("setBusStripPan",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPanLaw>("setBusStripPanLaw",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripPanMode>("setBusStripPanMode",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripDualPan>("setBusStripDualPan",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripChannelDelaySamples>(
              "setTrackStripChannelDelaySamples", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PopClipPageRequest>("popClipPageRequest",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetClipPagePrefetchFrames>(
              "setClipPagePrefetchFrames", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClipPagePrefetchFrames>("clipPagePrefetchFrames",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetCaptureBuffer>("setCaptureBuffer",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ArmCapture>("armCapture", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetCapturePunch>("setCapturePunch",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetCaptureSource>("setCaptureSource",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetRecordOffsetSamples>("setRecordOffsetSamples",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetInputMonitor>("setInputMonitor",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ResetCapture>("resetCapture", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CaptureStatus>("captureStatus", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::CapturedAudio>("capturedAudio", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetGraph>("setGraph", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::GraphNodeCount>("graphNodeCount",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::GraphConnectionCount>("graphConnectionCount",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Process>("process", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ProcessWithMonitor>("processWithMonitor",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::RenderOffline>("renderOffline", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::FinishOfflineRender>("finishOfflineRender",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::BounceOffline>("bounceOffline", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::FreezeOffline>("freezeOffline", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::DrainTelemetry>("drainTelemetry",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::DrainMeterTelemetry>("drainMeterTelemetry",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::DrainMeterTelemetryWide>("drainMeterTelemetryWide",
                                                                       napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MeterTargetInsertGainReduction>(
              "meterTargetInsertGainReduction", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ConfigureScopeTelemetry>("configureScopeTelemetry",
                                                                       napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::DrainScopeTelemetry>("drainScopeTelemetry",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetParameter>("setParameter", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetParameterSmoothed>("setParameterSmoothed",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetParamSmoothingMs>("setParamSmoothingMs",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetSoloMute>("setSoloMute", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetTrackMonitorMode>("setTrackMonitorMode",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearParameters>("clearParameters",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMidiClips>("setMidiClips", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetBuiltinInstrument>("setBuiltinInstrument",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetSynthInstrument>("setSynthInstrument",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::LoadSoundFont>("loadSoundFont", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetSf2Instrument>("setSf2Instrument",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInstrument>("clearMidiInstrument",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MidiInstrumentCount>("midiInstrumentCount",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::BindMidiCc>("bindMidiCc", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::BindMidiCcBinding>("bindMidiCcBinding",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiCcBindings>("clearMidiCcBindings",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MidiCcBindingCount>("midiCcBindingCount",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetControllerProfile>("setControllerProfile",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::BindController>("bindController",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearControllerBindings>("clearControllerBindings",
                                                                       napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ControllerBindingCount>("controllerBindingCount",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetControllerVelocityMeaningful>(
              "setControllerVelocityMeaningful", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ControllerVelocityMeaningful>(
              "controllerVelocityMeaningful", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetControllerNoteTracking>(
              "setControllerNoteTracking", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ControllerNoteTracking>("controllerNoteTracking",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetArticulation>("setArticulation",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Articulation>("articulation", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetPartRig>("setPartRig", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::LegatoFallbackCount>("legatoFallbackCount",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMidiFx>("setMidiFx", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiFx>("clearMidiFx", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMidiInputSource>("setMidiInputSource",
                                                                  napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInputSource>("clearMidiInputSource",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::MidiInputPendingCount>("midiInputPendingCount",
                                                                     napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOn>("pushMidiInputNoteOn",
                                                                   napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOff>("pushMidiInputNoteOff",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputCc>("pushMidiInputCc",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputPitchBend>("pushMidiInputPitchBend",
                                                                      napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputChannelPressure>(
              "pushMidiInputChannelPressure", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputPolyPressure>(
              "pushMidiInputPolyPressure", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOn>("pushMidiNoteOn",
                                                              napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOff>("pushMidiNoteOff",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiCc>("pushMidiCc", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPitchBend>("pushMidiPitchBend",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiChannelPressure>("pushMidiChannelPressure",
                                                                       napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPolyPressure>("pushMidiPolyPressure",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPanic>("pushMidiPanic", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiSysex>("pushMidiSysex", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiUmp>("pushMidiUmp", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputUmp>("pushMidiInputUmp",
                                                                napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiProgram>("pushMidiProgram",
                                                               napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputProgram>("pushMidiInputProgram",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetMidiDestinationExternal>(
              "setMidiDestinationExternal", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetExternalMidiClockEnabled>(
              "setExternalMidiClockEnabled", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ExternalMidiDroppedCount>("externalMidiDroppedCount",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::ClipPageRequestOverflowCount>(
              "clipPageRequestOverflowCount", napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::WarpStretchOverflowCount>("warpStretchOverflowCount",
                                                                        napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::SetWarpVoiceCapacity>("setWarpVoiceCapacity",
                                                                    napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::WarpVoiceCapacity>("warpVoiceCapacity",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::DrainExternalMidi>("drainExternalMidi",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::GetTransportState>("getTransportState",
                                                                 napi_default_method),
          InstanceMethod<&RealtimeEngineWrap::Destroy>("destroy", napi_default_method),
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
  // Absent arguments pass 0, which the C entry resolves to the library default.
  int max_block_size = 0;
  int64_t command_capacity = 0;
  int64_t telemetry_capacity = 0;
  int max_channels = 0;
  if (!OptionalIntArg(env, info, 1, "maxBlockSize", max_block_size, &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 0, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 0, &telemetry_capacity) ||
      !OptionalIntArg(env, info, 4, "maxChannels", 0, &max_channels)) {
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
  // The sample rate is required: an absent one reads as undefined and is
  // rejected by the reader below, so it reaches the caller as a TypeError rather
  // than as a silent no-op.
  double sample_rate = 0.0;
  if (!ReadEngineSampleRate(env, info[0], &sample_rate)) return env.Undefined();
  int max_block_size = 0;
  int64_t command_capacity = 0;
  int64_t telemetry_capacity = 0;
  int max_channels = 0;
  if (!OptionalIntArg(env, info, 1, "maxBlockSize", max_block_size, &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 0, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 0, &telemetry_capacity) ||
      !OptionalIntArg(env, info, 4, "maxChannels", 0, &max_channels)) {
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
  int block_size = 0;  // C value 0 = library default offline block
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !OptionalIntArg(env, info, 1, "blockSize", block_size, &block_size)) {
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

Napi::Value RealtimeEngineWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, engine_, "RealtimeEngine");
  int max_block = 0;
  ThrowIfError(env, sonare_engine_max_block_size(engine_, &max_block));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, max_block);
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
    segment.end_bpm = DoubleProperty(obj, "endBpm", segment.end_bpm);
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
    if (!RequiredIntProperty(env, obj, "curveToNext", &point.curve_to_next)) {
      return env.Undefined();
    }
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
    marker.key_minor = BoolProperty(obj, "keyMinor", marker.key_minor != 0) ? 1 : 0;
    if (!RequiredDoubleProperty(env, obj, "ppq", &marker.ppq)) return env.Undefined();
    if (!CopyString(env, marker.name, sizeof(marker.name), StringProperty(obj, "name", ""),
                    "marker name")) {
      return env.Undefined();
    }
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
  sonare_engine_metronome_config_init(&config);
  config.enabled = BoolProperty(obj, "enabled", config.enabled != 0) ? 1 : 0;
  config.beat_gain = FloatProperty(obj, "beatGain", config.beat_gain);
  config.accent_gain = FloatProperty(obj, "accentGain", config.accent_gain);
  config.click_samples =
      IntProperty(obj, "clickSamples", AbsentIsSentinel<int>{config.click_samples});
  config.click_sec = DoubleProperty(obj, "clickSeconds", config.click_sec);
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
