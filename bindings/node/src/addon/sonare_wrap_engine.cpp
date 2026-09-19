#include "sonare_wrap_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_sample_bank.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

bool ReadEngineBuiltinSynthConfig(Napi::Env env, const Napi::Object& obj,
                                  SonareEngineBuiltinSynthConfig* config) {
  if (!ReadBuiltinWaveform(env, obj.Get("waveform"), &config->waveform)) {
    return false;
  }
  config->gain = FloatProperty(obj, "gain", config->gain);
  config->attack_ms = FloatProperty(obj, "attackMs", config->attack_ms);
  config->decay_ms = FloatProperty(obj, "decayMs", config->decay_ms);
  config->sustain = FloatProperty(obj, "sustain", config->sustain);
  config->release_ms = FloatProperty(obj, "releaseMs", config->release_ms);
  config->polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
  // A wrong-typed field left a pending JS exception; stop before the caller
  // reaches the C ABI with it still set.
  return !env.IsExceptionPending();
}

// Controller-profile enum spellings, shared with the Python and WASM facades.
// The static_asserts catch a widened C enum; the spellings themselves are pinned
// against the C name tables by the enum-table export.
constexpr const char* kControllerInputs[] = {"control-change", "channel-pressure", "poly-pressure",
                                             "pitch-bend", "velocity"};
constexpr const char* kControllerAxes[] = {"none",  "excitation", "position",    "brightness",
                                           "morph", "loudness",   "pitch-cents", "vibrato-depth"};
static_assert(std::size(kControllerInputs) == SONARE_CONTROLLER_INPUT_COUNT,
              "Node ControllerInput table drifted from C");
static_assert(std::size(kControllerAxes) == SONARE_CONTROLLER_AXIS_COUNT,
              "Node ControllerAxis table drifted from C");

// Articulation spellings, shared with the Python and WASM facades.
constexpr const char* kArticulations[] = {"poly", "mono-retrigger", "mono-legato"};
static_assert(std::size(kArticulations) == SONARE_ARTICULATION_COUNT,
              "Node Articulation table drifted from C");

bool ReadControllerBinding(Napi::Env env, const Napi::Value& value, SonareControllerBinding* out) {
  if (!value.IsObject()) {
    Napi::TypeError::New(env, "controller binding must be an object").ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Object obj = value.As<Napi::Object>();
  // Both are required rather than defaulted: an omitted input would bind
  // control-change CC0, which the C ABI accepts as a binding that listens to
  // the wrong gesture.
  if (!sonare_node::SynthFieldPresent(obj, "input") ||
      !sonare_node::SynthFieldPresent(obj, "axis")) {
    Napi::TypeError::New(env, "controller binding requires an input and an axis")
        .ThrowAsJavaScriptException();
    return false;
  }
  int input = 0;
  int axis = 0;
  if (!sonare_node::SynthEnumProperty(env, obj, "input", kControllerInputs,
                                      SONARE_CONTROLLER_INPUT_COUNT, "controller input", &input) ||
      !sonare_node::SynthEnumProperty(env, obj, "axis", kControllerAxes,
                                      SONARE_CONTROLLER_AXIS_COUNT, "controller axis", &axis)) {
    return false;
  }
  out->input = static_cast<uint8_t>(input);
  out->axis = static_cast<uint8_t>(axis);
  out->index = MidiByteProperty(env, obj, "index", 0);
  out->lo = sonare_node::FiniteFloatProperty(obj, "lo", 0.0f);
  out->hi = sonare_node::FiniteFloatProperty(obj, "hi", 1.0f);
  out->curve = sonare_node::FiniteFloatProperty(obj, "curve", 1.0f);
  return !env.IsExceptionPending();
}

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
  if (!std::isfinite(sample_rate) || sample_rate < 8000.0 || sample_rate > 384000.0) {
    Napi::RangeError::New(env, "sampleRate must be finite and within 8000..384000 Hz")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = sample_rate;
  return true;
}

std::vector<SonareEngineMidiEvent> ReadEngineMidiEvents(Napi::Env env, Napi::Value value) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "MIDI clip events must be an array").ThrowAsJavaScriptException();
    return {};
  }
  Napi::Array input = value.As<Napi::Array>();
  std::vector<SonareEngineMidiEvent> events(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value item = input.Get(i);
    if (!item.IsObject()) {
      Napi::TypeError::New(env, "MIDI clip events must be objects").ThrowAsJavaScriptException();
      return {};
    }
    Napi::Object obj = item.As<Napi::Object>();
    SonareEngineMidiEvent event{};
    event.render_frame = Int64Property(obj, "renderFrame", 0);
    event.word0 = WordProperty(obj, "word0", WordProperty(obj, "data0", 0));
    event.word1 = WordProperty(obj, "word1", WordProperty(obj, "data1", 0));
    event.word2 = WordProperty(obj, "word2", 0);
    event.word3 = WordProperty(obj, "word3", 0);
    // wordCount is documented as tolerant: anything outside [1,4] lets the C
    // bridge infer the word form. Narrow out of range rather than through the
    // uint8_t cast, which would land 257 on a spurious 1 instead of inferring.
    const uint32_t word_count = Uint32Property(obj, "wordCount", 0);
    event.word_count = word_count >= 1 && word_count <= 4 ? static_cast<uint8_t>(word_count) : 0;
    event.group = MidiByteProperty(env, obj, "group", 0);
    event.sysex_handle = Uint32Property(obj, "sysexHandle", 0);
    // Bail on the first wrong-typed field so the next iteration's explicit
    // TypeError is never raised on top of an already-pending exception.
    if (env.IsExceptionPending()) return {};
    events[i] = event;
  }
  return events;
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
          InstanceMethod<&RealtimeEngineWrap::SeekPpq>("seekPpq"),
          InstanceMethod<&RealtimeEngineWrap::SetTempo>("setTempo"),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignature>("setTimeSignature"),
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
          InstanceMethod<&RealtimeEngineWrap::ClipCount>("clipCount"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackLanes>("setTrackLanes"),
          InstanceMethod<&RealtimeEngineWrap::SetLaneSidechain>("setLaneSidechain"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackBuses>("setTrackBuses"),
          InstanceMethod<&RealtimeEngineWrap::SetBusStripJson>("setBusStripJson"),
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
          InstanceMethod<&RealtimeEngineWrap::SetBusStripInsertBypassed>(
              "setBusStripInsertBypassed"),
          InstanceMethod<&RealtimeEngineWrap::ResolveTrackInsertAutomationId>(
              "resolveTrackInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveMasterInsertAutomationId>(
              "resolveMasterInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveBusInsertAutomationId>(
              "resolveBusInsertAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::ResolveInstrumentAutomationId>(
              "resolveInstrumentAutomationId"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPan>("setTrackStripPan"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanLaw>("setTrackStripPanLaw"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripPanMode>("setTrackStripPanMode"),
          InstanceMethod<&RealtimeEngineWrap::SetTrackStripDualPan>("setTrackStripDualPan"),
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
          InstanceMethod<&RealtimeEngineWrap::SetArticulation>("setArticulation"),
          InstanceMethod<&RealtimeEngineWrap::Articulation>("articulation"),
          InstanceMethod<&RealtimeEngineWrap::LegatoFallbackCount>("legatoFallbackCount"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiFx>("setMidiFx"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiFx>("clearMidiFx"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiInputSource>("setMidiInputSource"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInputSource>("clearMidiInputSource"),
          InstanceMethod<&RealtimeEngineWrap::MidiInputPendingCount>("midiInputPendingCount"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOn>("pushMidiInputNoteOn"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputNoteOff>("pushMidiInputNoteOff"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiInputCc>("pushMidiInputCc"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOn>("pushMidiNoteOn"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiNoteOff>("pushMidiNoteOff"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiCc>("pushMidiCc"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiPanic>("pushMidiPanic"),
          InstanceMethod<&RealtimeEngineWrap::PushMidiSysex>("pushMidiSysex"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiDestinationExternal>(
              "setMidiDestinationExternal"),
          InstanceMethod<&RealtimeEngineWrap::SetExternalMidiClockEnabled>(
              "setExternalMidiClockEnabled"),
          InstanceMethod<&RealtimeEngineWrap::ExternalMidiDroppedCount>("externalMidiDroppedCount"),
          InstanceMethod<&RealtimeEngineWrap::ClipPageRequestOverflowCount>(
              "clipPageRequestOverflowCount"),
          InstanceMethod<&RealtimeEngineWrap::WarpStretchOverflowCount>("warpStretchOverflowCount"),
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
    Napi::Error::New(env, "libsonare engine ABI mismatch: native binary reports version " +
                              std::to_string(abi_version) + ", expected " +
                              std::to_string(kExpectedEngineAbiVersion) +
                              ". The prebuilt addon is incompatible with this binding.")
        .ThrowAsJavaScriptException();
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
  // providers are released next. Entries are nulled by destroyClipPageProvider,
  // which is what makes a second pass here a no-op rather than a double free.
  for (SonareClipPageProvider* provider : clip_page_providers_) {
    if (provider != nullptr) sonare_clip_page_provider_destroy(provider);
  }
  clip_page_providers_.clear();
  clip_page_providers_.shrink_to_fit();
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
  if (engine_ == nullptr) {
    Napi::Error::New(env, "RealtimeEngine is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Both leading arguments are required: an absent one reads as undefined and is
  // rejected by the reader below, so it reaches the caller as a TypeError rather
  // than as a silent no-op.
  double sample_rate = 0.0;
  if (!ReadEngineSampleRate(env, info[0], &sample_rate)) return env.Undefined();
  int max_block_size = 0;
  int64_t command_capacity = 1024;
  int64_t telemetry_capacity = 1024;
  if (!RequiredIntArg(env, info, 1, "maxBlockSize", &max_block_size) ||
      !OptionalInt64Arg(env, info, 2, "commandCapacity", 1024, &command_capacity) ||
      !OptionalInt64Arg(env, info, 3, "telemetryCapacity", 1024, &telemetry_capacity)) {
    return env.Undefined();
  }
  const int max_channels = node_arg_int(info, 4, 64);
  ThrowIfError(env, sonare_engine_prepare_with_channels(
                        engine_, sample_rate, max_block_size, static_cast<size_t>(command_capacity),
                        static_cast<size_t>(telemetry_capacity), max_channels));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Play(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_play(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Stop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_stop(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  ThrowIfError(env, sonare_engine_settle_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::FlushControlCommands(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_flush_control_commands(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const double ppq = node_arg_double(info, 0, 0.0);
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_seek_ppq(engine_, ppq, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const double bpm = node_arg_double(info, 0, 120.0);
  ThrowIfError(env, sonare_engine_set_tempo(engine_, bpm));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignature(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const int numerator = node_arg_int(info, 0, 4);
  const int denominator = node_arg_int(info, 1, 4);
  ThrowIfError(env, sonare_engine_set_time_signature(engine_, numerator, denominator));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTempoSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<SonareProjectTempoSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
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
  }
  ThrowIfError(env, sonare_engine_set_tempo_segments(engine_, segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTimeSignatureSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<SonareProjectTimeSignatureSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
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
  }
  ThrowIfError(
      env, sonare_engine_set_time_signature_segments(engine_, segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SampleAtPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const double ppq = node_arg_double(info, 0, 0.0);
  int64_t sample = 0;
  ThrowIfError(env, sonare_engine_sample_at_ppq(engine_, ppq, &sample));
  return Napi::Number::New(env, static_cast<double>(sample));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const double start_ppq = node_arg_double(info, 0, 0.0);
  const double end_ppq = node_arg_double(info, 1, 0.0);
  bool enabled = true;
  if (!OptionalBoolArg(env, info, 2, "enabled", true, &enabled)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_loop(engine_, start_ppq, end_ppq, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::AddParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareParameterInfo parameter{};
  if (!ReadParameter(info, 0, &parameter)) return env.Undefined();
  ThrowIfError(env, sonare_engine_add_parameter(engine_, &parameter));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ParameterCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t count = 0;
  ThrowIfError(env, sonare_engine_parameter_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ParameterInfoByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  const uint32_t id = node_arg_uint32(info, 0, 0);
  SonareParameterInfo parameter{};
  ThrowIfError(env, sonare_engine_parameter_info(engine_, id, &parameter));
  if (env.IsExceptionPending()) return env.Undefined();
  return ParameterToObject(env, parameter);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    point.ppq = obj.Get("ppq").As<Napi::Number>().DoubleValue();
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
  size_t count = 0;
  ThrowIfError(env, sonare_engine_automation_lane_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    marker.kind = static_cast<uint8_t>(IntProperty(obj, "kind", SONARE_MARKER_KIND_MARKER));
    marker.key_fifths = static_cast<int8_t>(IntProperty(obj, "keyFifths", 0));
    marker.key_minor = BoolProperty(obj, "keyMinor", false) ? 1 : 0;
    marker.ppq = obj.Get("ppq").As<Napi::Number>().DoubleValue();
    const Napi::Value name = obj.Get("name");
    CopyString(marker.name, sizeof(marker.name),
               name.IsUndefined() || name.IsNull() ? "" : name.As<Napi::String>().Utf8Value());
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
  size_t count = 0;
  ThrowIfError(env, sonare_engine_marker_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MarkerByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  const uint32_t id = node_arg_uint32(info, 0, 0);
  SonareEngineMarker marker{};
  ThrowIfError(env, sonare_engine_marker(engine_, id, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  return MarkerToObject(env, marker);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SeekMarker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t id = node_arg_uint32(info, 0, 0);
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 1, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_seek_marker(engine_, id, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLoopFromMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t start_id = node_arg_uint32(info, 0, 0);
  const uint32_t end_id = node_arg_uint32(info, 1, 0);
  ThrowIfError(env, sonare_engine_set_loop_from_markers(engine_, start_id, end_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMetronome(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SonareEngineMetronomeConfig config{};
  ThrowIfError(env, sonare_engine_metronome(engine_, &config));
  if (env.IsExceptionPending()) return env.Undefined();
  return MetronomeToObject(env, config);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CountInEndSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t start_sample = 0;
  if (!OptionalInt64Arg(env, info, 0, "startSample", 0, &start_sample)) return env.Undefined();
  const int bars = node_arg_int(info, 1, 1);
  int64_t out = 0;
  ThrowIfError(env, sonare_engine_count_in_end_sample(engine_, start_sample, bars, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t param_id = node_arg_uint32(info, 0, 0);
  const float value = node_arg_float(info, 1, 0.0f);
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_parameter(engine_, param_id, value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParameterSmoothed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t param_id = node_arg_uint32(info, 0, 0);
  const float value = node_arg_float(info, 1, 0.0f);
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_parameter_smoothed(engine_, param_id, value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetParamSmoothingMs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const float smoothing_ms = node_arg_float(info, 0, 0.0f);
  ThrowIfError(env, sonare_engine_set_param_smoothing_ms(engine_, smoothing_ms));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetSoloMute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t lane_index = node_arg_uint32(info, 0, 0);
  const bool solo = node_arg_bool(info, 1, false);
  const bool mute = node_arg_bool(info, 2, false);
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 3, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(
      env, sonare_engine_set_solo_mute(engine_, lane_index, solo ? 1 : 0, mute ? 1 : 0, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackMonitorMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t lane_index = node_arg_uint32(info, 0, 0);
  const auto mode = static_cast<SonareEngineTrackMonitorMode>(node_arg_int(info, 1, 0));
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_track_monitor_mode(engine_, lane_index, mode, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_clear_parameters(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() == 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "setMidiClips expects an array of clip schedules")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<std::vector<SonareEngineMidiEvent>> event_storage(input.Length());
  std::vector<SonareEngineMidiClipSchedule> clips(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value item = input.Get(i);
    if (!item.IsObject()) {
      Napi::TypeError::New(env, "MIDI clips must be objects").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = item.As<Napi::Object>();
    event_storage[i] =
        ReadEngineMidiEvents(env, obj.Has("events") ? obj.Get("events") : env.Null());
    if (env.IsExceptionPending()) return env.Undefined();
    SonareEngineMidiClipSchedule clip{};
    clip.id = Uint32Property(obj, "id", 0);
    clip.track_id = Uint32Property(obj, "trackId", 0);
    clip.start_sample = Int64Property(obj, "startSample", 0);
    clip.start_ppq = DoubleProperty(obj, "startPpq", 0.0);
    clip.length_samples = Int64Property(obj, "lengthSamples", kZeroIsSentinel);
    clip.loop = obj.Get("loop").ToBoolean().Value() ? 1 : 0;
    clip.loop_length_samples = Int64Property(obj, "loopLengthSamples", kZeroIsSentinel);
    clip.destination_id = Uint32Property(obj, "destinationId", Uint32Property(obj, "trackId", 0));
    if (env.IsExceptionPending()) return env.Undefined();
    clip.events = event_storage[i].data();
    clip.event_count = event_storage[i].size();
    clips[i] = clip;
  }
  ThrowIfError(env, sonare_engine_set_midi_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBuiltinInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  SonareEngineBuiltinSynthConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    if (!ReadEngineBuiltinSynthConfig(env, obj, &config)) return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_builtin_instrument(engine_, destination_id, &config));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// Binds the patch-driven NativeSynth to a realtime MIDI destination:
//   setSynthInstrument(destinationId, patch)
// where `patch` is a SynthPatch object or a preset-name string ("saw-lead" /
// "va:saw-lead"), resolving exactly like Project.bounceWithSynthInstruments. A
// sample patch carries its bank as the descriptor's `sampleBank`, the same key
// the bounce reads; the engine takes a share of it.
Napi::Value RealtimeEngineWrap::SetSynthInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  SonareSynthPatch patch{};
  SonareSampleBank* bank = nullptr;
  if (info.Length() > 1) {
    if (!sonare_node::ReadSynthPatch(env, info[1], &patch)) {
      return env.Undefined();  // exception already pending
    }
    if (info[1].IsObject() && !info[1].IsArray() &&
        !SampleBankWrap::ReadHandle(env, info[1].As<Napi::Object>().Get("sampleBank"), &bank)) {
      return env.Undefined();  // exception already pending
    }
  } else {
    patch.struct_version = 3;
  }
  ThrowIfError(env,
               sonare_engine_set_synth_instrument_with_bank(engine_, destination_id, &patch, bank));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::LoadSoundFont(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 0 && sonare_node::IsUint8Array(info[0])) {
    Napi::Uint8Array arr = info[0].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "loadSoundFont expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_load_soundfont(engine_, bytes, len));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetSf2Instrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  SonareEngineSf2InstrumentConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    config.gain = FloatProperty(obj, "gain", config.gain);
    config.polyphony = IntProperty(obj, "polyphony", kZeroIsSentinel);
    const Napi::Value prefer_model = obj.Get("preferModelForModeledFamilies");
    if (!prefer_model.IsUndefined() && !prefer_model.IsNull()) {
      config.struct_version = 2;
      config.prefer_model_for_modeled_families = prefer_model.ToBoolean().Value() ? 1 : 0;
    }
    // Version 3 reads version 2's field as well, so raising it here covers both
    // whichever of the two the caller passed.
    const Napi::Value clear_rig = obj.Get("clearBankRig");
    if (!clear_rig.IsUndefined() && !clear_rig.IsNull()) {
      config.struct_version = 3;
      config.clear_bank_rig = clear_rig.ToBoolean().Value() ? 1 : 0;
    }
    if (env.IsExceptionPending()) return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_sf2_instrument(engine_, destination_id, &config));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  ThrowIfError(env, sonare_engine_clear_midi_instrument(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiInstrumentCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_instrument_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t deadline = -1;
  if (!OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 4, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_note_on(engine_, destination_id, group, channel, note,
                                                    velocity, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::BindMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t channel = 0;
  uint8_t controller = 0;
  if (!OptionalMidiByteArg(env, info, 0, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 1, "controller", 0, &controller)) {
    return env.Undefined();
  }
  const uint32_t param_id = node_arg_uint32(info, 2, 0);
  const float min_value = node_arg_float(info, 3, 0.0f);
  const float max_value = node_arg_float(info, 4, 1.0f);
  ThrowIfError(env, sonare_engine_bind_midi_cc(engine_, channel, controller, param_id, min_value,
                                               max_value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::BindMidiCcBinding(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected a MIDI CC binding object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object object = info[0].As<Napi::Object>();
  const Napi::Value cc_number = object.Get("ccNumber");
  const Napi::Value param_id = object.Get("paramId");
  if (!cc_number.IsNumber() || !param_id.IsNumber()) {
    Napi::TypeError::New(env, "MIDI CC binding requires numeric ccNumber and paramId")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareMidiCcBinding binding{};
  // Every byte-wide field goes through the MidiByte readers: the C-ABI struct
  // stores them as uint8_t, so a plain Uint32Value() would let 256 arrive as 0
  // and 271 as 15 — inside the range the C ABI's own check accepts.
  if (!RequiredMidiByteValue(env, cc_number, "ccNumber", &binding.cc_number)) {
    return env.Undefined();
  }
  // `channel` keeps its "any channel" sentinel for an omitted, null, or
  // undefined value.
  binding.channel = MidiByteProperty(env, object, "channel", 0xffu);
  binding.kind = MidiByteProperty(env, object, "kind", 0u);
  binding.cc_lsb_number = MidiByteProperty(env, object, "ccLsbNumber", 0u);
  binding.selector_msb = MidiByteProperty(env, object, "selectorMsb", 0u);
  binding.selector_lsb = MidiByteProperty(env, object, "selectorLsb", 0u);
  if (env.IsExceptionPending()) return env.Undefined();
  binding.param_id = node_narrow_uint32(env, param_id, "paramId");
  binding.min_value = FloatProperty(object, "minValue", 0.0f);
  binding.max_value = FloatProperty(object, "maxValue", 1.0f);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_bind_midi_cc_binding(engine_, &binding));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiCcBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_clear_midi_cc_bindings(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiCcBindingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_cc_binding_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerProfile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  if (info.Length() < 2 || !info[1].IsString()) {
    Napi::TypeError::New(env, "setControllerProfile expects a preset name string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string preset = info[1].As<Napi::String>().Utf8Value();
  ThrowIfError(env, sonare_engine_set_controller_profile(engine_, destination_id, preset.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::BindController(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  SonareControllerBinding binding{};
  if (!ReadControllerBinding(env, info.Length() > 1 ? info[1] : env.Undefined(), &binding)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_bind_controller(engine_, destination_id, &binding));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearControllerBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  ThrowIfError(env, sonare_engine_clear_controller_bindings(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ControllerBindingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  size_t count = 0;
  ThrowIfError(env, sonare_engine_controller_binding_count(engine_, destination_id, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetControllerVelocityMeaningful(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  const bool meaningful = node_arg_bool(info, 1, false);
  ThrowIfError(env, sonare_engine_set_controller_velocity_meaningful(engine_, destination_id,
                                                                     meaningful ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ControllerVelocityMeaningful(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  int meaningful = 0;
  ThrowIfError(env,
               sonare_engine_controller_velocity_meaningful(engine_, destination_id, &meaningful));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Boolean::New(env, meaningful != 0);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetArticulation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint8_t channel = 0;
  if (!OptionalMidiByteArg(env, info, 1, "channel", 0, &channel)) return env.Undefined();
  // Required rather than defaulted, for the reason the C ABI refuses an ordinal
  // past the enum instead of clamping it: poly substituted for a misspelled
  // mono-legato plays every note and slurs none of them, which sounds exactly
  // like a request that took.
  int articulation = SONARE_ARTICULATION_POLY;
  if (!sonare_node::SynthEnumValue(env, info[2], kArticulations, SONARE_ARTICULATION_COUNT,
                                   "articulation", &articulation)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_articulation(engine_, destination_id, channel, articulation));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::Articulation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint8_t channel = 0;
  if (!OptionalMidiByteArg(env, info, 1, "channel", 0, &channel)) return env.Undefined();
  int articulation = 0;
  ThrowIfError(env, sonare_engine_articulation(engine_, destination_id, channel, &articulation));
  if (env.IsExceptionPending()) return env.Undefined();
  return sonare_node::SynthEnumName(env, articulation, kArticulations, SONARE_ARTICULATION_COUNT);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::LegatoFallbackCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_legato_fallback_count(engine_, destination_id, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  ThrowIfError(env, sonare_engine_set_midi_fx(engine_, destination_id, config.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  ThrowIfError(env, sonare_engine_clear_midi_fx(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  ThrowIfError(env, sonare_engine_set_midi_input_source(engine_, destination_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_clear_midi_input_source(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::MidiInputPendingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_input_pending_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 3, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 4, "portTimeSamples", 0, &port_time)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_note_on(engine_, group, channel, note, velocity,
                                                          port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 3, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 4, "portTimeSamples", 0, &port_time)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_input_note_off(engine_, group, channel, note, velocity,
                                                           port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiInputCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t controller = 0;
  uint8_t value = 0;
  int64_t port_time = 0;
  if (!OptionalMidiByteArg(env, info, 0, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 1, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 2, "controller", 0, &controller) ||
      !OptionalMidiByteArg(env, info, 3, "value", 0, &value) ||
      !OptionalInt64Arg(env, info, 4, "portTimeSamples", 0, &port_time)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_push_midi_input_cc(engine_, group, channel, controller, value, port_time));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  int64_t deadline = -1;
  if (!OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "note", 0, &note) ||
      !OptionalMidiByteArg(env, info, 4, "velocity", 0, &velocity) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_note_off(engine_, destination_id, group, channel, note,
                                                     velocity, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t controller = 0;
  uint8_t value = 0;
  int64_t deadline = -1;
  if (!OptionalMidiByteArg(env, info, 1, "group", 0, &group) ||
      !OptionalMidiByteArg(env, info, 2, "channel", 0, &channel) ||
      !OptionalMidiByteArg(env, info, 3, "controller", 0, &controller) ||
      !OptionalMidiByteArg(env, info, 4, "value", 0, &value) ||
      !OptionalInt64Arg(env, info, 5, "renderFrame", -1, &deadline)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_push_midi_cc(engine_, destination_id, group, channel, controller,
                                               value, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiPanic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 0, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_push_midi_panic(engine_, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PushMidiSysex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 1 && info[1].IsBuffer()) {
    Napi::Buffer<uint8_t> buf = info[1].As<Napi::Buffer<uint8_t>>();
    bytes = buf.Data();
    len = buf.Length();
  } else if (info.Length() > 1 && sonare_node::IsUint8Array(info[1])) {
    Napi::Uint8Array arr = info[1].As<Napi::Uint8Array>();
    bytes = arr.Data();
    len = arr.ByteLength();
  } else {
    Napi::TypeError::New(env, "pushMidiSysex expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int64_t deadline = -1;
  if (!OptionalInt64Arg(env, info, 2, "renderFrame", -1, &deadline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_push_midi_sysex(engine_, destination_id, bytes, len, deadline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMidiDestinationExternal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint32_t destination_id = node_arg_uint32(info, 0, 0);
  const bool external = node_arg_bool(info, 1, false);
  ThrowIfError(
      env, sonare_engine_set_midi_destination_external(engine_, destination_id, external ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetExternalMidiClockEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const bool enabled = node_arg_bool(info, 0, false);
  ThrowIfError(env, sonare_engine_set_external_midi_clock_enabled(engine_, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ExternalMidiDroppedCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_external_midi_dropped_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClipPageRequestOverflowCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_clip_page_request_overflow_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::WarpStretchOverflowCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t count = 0;
  ThrowIfError(env, sonare_engine_warp_stretch_overflow_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

// Drains queued external-MIDI events, already lowered to MIDI 1.0 byte
// messages. Each returned item is { destinationId, renderFrame, bytes:
// number[] }; transport/clock bytes carry destinationId === 0xFFFFFFFF. A
// single queued channel-voice event may lower to more than one item. @p
// maxRecords caps the number of output events produced (the unit shared by
// every surface) and goes through the same domain check as the telemetry
// drains. The C-ABI capacity passed per call is clamped to the remaining budget
// so the destructive drain never consumes more events than it can return —
// events that do not fit stay queued for the next call (lossless). A budget
// below kMaxLoweredMessages could never consume a record, so it is rejected
// rather than draining nothing while the queue keeps growing.
Napi::Value RealtimeEngineWrap::DrainExternalMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // The most MIDI-1 messages one queue record can lower to, and hence the
  // smallest capacity the C ABI accepts.
  constexpr size_t kMaxLoweredMessages = 3;
  constexpr uint32_t kMaxBytes =
      static_cast<uint32_t>(std::extent<decltype(SonareExternalMidiEvent::bytes)>::value);
  size_t max_records = 1024;
  if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!NonNegativeSizeTArg(env, info, 0, "maxRecords", &max_records)) return env.Undefined();
  }
  if (max_records > 0 && max_records < kMaxLoweredMessages) {
    Napi::RangeError::New(env, "maxRecords must be 0 or at least " +
                                   std::to_string(kMaxLoweredMessages) +
                                   " to guarantee forward progress")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env);
  if (max_records == 0) return out;
  std::array<SonareExternalMidiEvent, 256> records{};
  size_t out_index = 0;
  while (out_index + kMaxLoweredMessages <= max_records) {
    const size_t want = std::min(records.size(), max_records - out_index);
    size_t written = 0;
    const SonareError err =
        sonare_engine_drain_external_midi(engine_, records.data(), want, &written);
    ThrowIfError(env, err);
    if (env.IsExceptionPending()) return env.Undefined();
    if (written == 0) break;
    // Defensive: the C ABI promises written <= want, but clamp anyway so a
    // misreporting drain cannot read past the buffer or overrun the budget.
    written = std::min(written, want);
    for (size_t i = 0; i < written; ++i) {
      const SonareExternalMidiEvent& rec = records[i];
      Napi::Object item = Napi::Object::New(env);
      item.Set("destinationId", Napi::Number::New(env, static_cast<double>(rec.destination_id)));
      item.Set("renderFrame", Napi::Number::New(env, static_cast<double>(rec.render_frame)));
      // byte_count is documented as 1..3, but it indexes a fixed-size array:
      // bound it by the array extent before reading through it.
      const uint32_t byte_count = std::min(rec.byte_count, kMaxBytes);
      Napi::Array bytes = Napi::Array::New(env, byte_count);
      for (uint32_t b = 0; b < byte_count; ++b) {
        bytes.Set(b, Napi::Number::New(env, rec.bytes[b]));
      }
      item.Set("bytes", bytes);
      out.Set(static_cast<uint32_t>(out_index++), item);
    }
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::GetTransportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
