#include "sonare_wrap_engine.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_synth_patch.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

int WaveformFromName(const std::string& name) {
  if (name == "sine") return SONARE_SYNTH_WAVEFORM_SINE;
  if (name == "saw" || name == "sawtooth") return SONARE_SYNTH_WAVEFORM_SAW;
  if (name == "square") return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (name == "triangle") return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

bool ReadEngineBuiltinSynthConfig(Napi::Env env, const Napi::Object& obj,
                                  SonareEngineBuiltinSynthConfig* config) {
  Napi::Value waveform = obj.Get("waveform");
  if (waveform.IsString()) {
    const std::string name = waveform.As<Napi::String>().Utf8Value();
    const int mapped = WaveformFromName(name);
    if (mapped < 0) {
      Napi::TypeError::New(env, "Unknown synth waveform name: '" + name +
                                    "' (expected sine, saw, square, or triangle)")
          .ThrowAsJavaScriptException();
      return false;
    }
    config->waveform = mapped;
  } else if (waveform.IsNumber()) {
    config->waveform = waveform.As<Napi::Number>().Int32Value();
  }
  if (obj.Has("gain")) config->gain = obj.Get("gain").As<Napi::Number>().FloatValue();
  if (obj.Has("attackMs")) config->attack_ms = obj.Get("attackMs").As<Napi::Number>().FloatValue();
  if (obj.Has("decayMs")) config->decay_ms = obj.Get("decayMs").As<Napi::Number>().FloatValue();
  if (obj.Has("sustain")) config->sustain = obj.Get("sustain").As<Napi::Number>().FloatValue();
  if (obj.Has("releaseMs"))
    config->release_ms = obj.Get("releaseMs").As<Napi::Number>().FloatValue();
  if (obj.Has("polyphony"))
    config->polyphony = obj.Get("polyphony").As<Napi::Number>().Int32Value();
  return true;
}

uint32_t OptionalUint32(const Napi::Object& obj, const char* key, uint32_t fallback) {
  return obj.Has(key) ? obj.Get(key).As<Napi::Number>().Uint32Value() : fallback;
}

int64_t OptionalInt64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  return obj.Has(key) ? obj.Get(key).As<Napi::Number>().Int64Value() : fallback;
}

double OptionalDoubleProperty(const Napi::Object& obj, const char* key, double fallback) {
  return obj.Has(key) ? obj.Get(key).As<Napi::Number>().DoubleValue() : fallback;
}

uint32_t MidiWordFromObject(const Napi::Object& obj, const char* key, uint32_t fallback) {
  return obj.Has(key) ? obj.Get(key).As<Napi::Number>().Uint32Value() : fallback;
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
    event.render_frame = OptionalInt64Property(obj, "renderFrame", 0);
    event.word0 = MidiWordFromObject(obj, "word0", MidiWordFromObject(obj, "data0", 0));
    event.word1 = MidiWordFromObject(obj, "word1", MidiWordFromObject(obj, "data1", 0));
    event.word2 = MidiWordFromObject(obj, "word2", 0);
    event.word3 = MidiWordFromObject(obj, "word3", 0);
    event.word_count = static_cast<uint8_t>(OptionalUint32(obj, "wordCount", 0));
    event.group = static_cast<uint8_t>(OptionalUint32(obj, "group", 0));
    event.sysex_handle = OptionalUint32(obj, "sysexHandle", 0);
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
          InstanceMethod<&RealtimeEngineWrap::SeekPpq>("seekPpq"),
          InstanceMethod<&RealtimeEngineWrap::SetTempo>("setTempo"),
          InstanceMethod<&RealtimeEngineWrap::SetTimeSignature>("setTimeSignature"),
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
          InstanceMethod<&RealtimeEngineWrap::CreateClipPageProvider>("createClipPageProvider"),
          InstanceMethod<&RealtimeEngineWrap::SupplyClipPage>("supplyClipPage"),
          InstanceMethod<&RealtimeEngineWrap::ClearClipPage>("clearClipPage"),
          InstanceMethod<&RealtimeEngineWrap::DestroyClipPageProvider>("destroyClipPageProvider"),
          InstanceMethod<&RealtimeEngineWrap::PopClipPageRequest>("popClipPageRequest"),
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
          InstanceMethod<&RealtimeEngineWrap::BounceOffline>("bounceOffline"),
          InstanceMethod<&RealtimeEngineWrap::FreezeOffline>("freezeOffline"),
          InstanceMethod<&RealtimeEngineWrap::DrainTelemetry>("drainTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::DrainMeterTelemetry>("drainMeterTelemetry"),
          InstanceMethod<&RealtimeEngineWrap::SetParameter>("setParameter"),
          InstanceMethod<&RealtimeEngineWrap::SetParameterSmoothed>("setParameterSmoothed"),
          InstanceMethod<&RealtimeEngineWrap::SetSoloMute>("setSoloMute"),
          InstanceMethod<&RealtimeEngineWrap::ClearParameters>("clearParameters"),
          InstanceMethod<&RealtimeEngineWrap::SetMidiClips>("setMidiClips"),
          InstanceMethod<&RealtimeEngineWrap::SetBuiltinInstrument>("setBuiltinInstrument"),
          InstanceMethod<&RealtimeEngineWrap::SetSynthInstrument>("setSynthInstrument"),
          InstanceMethod<&RealtimeEngineWrap::LoadSoundFont>("loadSoundFont"),
          InstanceMethod<&RealtimeEngineWrap::SetSf2Instrument>("setSf2Instrument"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiInstrument>("clearMidiInstrument"),
          InstanceMethod<&RealtimeEngineWrap::MidiInstrumentCount>("midiInstrumentCount"),
          InstanceMethod<&RealtimeEngineWrap::BindMidiCc>("bindMidiCc"),
          InstanceMethod<&RealtimeEngineWrap::ClearMidiCcBindings>("clearMidiCcBindings"),
          InstanceMethod<&RealtimeEngineWrap::MidiCcBindingCount>("midiCcBindingCount"),
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
          InstanceMethod<&RealtimeEngineWrap::GetTransportState>("getTransportState"),
          InstanceMethod<&RealtimeEngineWrap::Destroy>("destroy"),
      });
  exports.Set("RealtimeEngine", func);
  return exports;
}

RealtimeEngineWrap::RealtimeEngineWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RealtimeEngineWrap>(info) {
  Napi::Env env = info.Env();
  const uint32_t abi_version = sonare_engine_abi_version();
  if (abi_version != kExpectedEngineAbiVersion) {
    Napi::Error::New(env, "libsonare engine ABI mismatch: native binary reports version " +
                              std::to_string(abi_version) + ", expected " +
                              std::to_string(kExpectedEngineAbiVersion) +
                              ". The prebuilt addon is incompatible with this binding.")
        .ThrowAsJavaScriptException();
    return;
  }
  const double sample_rate = info.Length() > 0 && !info[0].IsUndefined()
                                 ? info[0].As<Napi::Number>().DoubleValue()
                                 : 48000.0;
  const int max_block_size =
      info.Length() > 1 && !info[1].IsUndefined() ? info[1].As<Napi::Number>().Int32Value() : 128;
  const size_t command_capacity = info.Length() > 2 && !info[2].IsUndefined()
                                      ? static_cast<size_t>(info[2].As<Napi::Number>().Int64Value())
                                      : 1024;
  const size_t telemetry_capacity =
      info.Length() > 3 && !info[3].IsUndefined()
          ? static_cast<size_t>(info[3].As<Napi::Number>().Int64Value())
          : 1024;

  SonareError err = sonare_engine_create(&engine_);
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return;
  err = sonare_engine_prepare(engine_, sample_rate, max_block_size, command_capacity,
                              telemetry_capacity);
  ThrowIfError(env, err);
}

RealtimeEngineWrap::~RealtimeEngineWrap() {
  for (SonareClipPageProvider* provider : clip_page_providers_) {
    sonare_clip_page_provider_destroy(provider);
  }
  clip_page_providers_.clear();
  if (engine_ != nullptr) {
    sonare_engine_destroy(engine_);
    engine_ = nullptr;
  }
}

Napi::Value RealtimeEngineWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (engine_ == nullptr) {
    Napi::Error::New(env, "RealtimeEngine is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const double sample_rate = info[0].As<Napi::Number>().DoubleValue();
  const int max_block_size = info[1].As<Napi::Number>().Int32Value();
  const size_t command_capacity =
      info.Length() > 2 ? static_cast<size_t>(info[2].As<Napi::Number>().Int64Value()) : 1024;
  const size_t telemetry_capacity =
      info.Length() > 3 ? static_cast<size_t>(info[3].As<Napi::Number>().Int64Value()) : 1024;
  ThrowIfError(env, sonare_engine_prepare(engine_, sample_rate, max_block_size, command_capacity,
                                          telemetry_capacity));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::Play(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_play(engine_, OptionalInt64(info, 0, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::Stop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_stop(engine_, OptionalInt64(info, 0, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SeekSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_seek_sample(engine_, OptionalInt64(info, 0, 0),
                                              OptionalInt64(info, 1, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SeekPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const double ppq = info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 0.0;
  ThrowIfError(env, sonare_engine_seek_ppq(engine_, ppq, OptionalInt64(info, 1, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTempo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const double bpm = info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 120.0;
  ThrowIfError(env, sonare_engine_set_tempo(engine_, bpm));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTimeSignature(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int numerator = info.Length() > 0 ? info[0].As<Napi::Number>().Int32Value() : 4;
  const int denominator = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 4;
  ThrowIfError(env, sonare_engine_set_time_signature(engine_, numerator, denominator));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SampleAtPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const double ppq = info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 0.0;
  int64_t sample = 0;
  ThrowIfError(env, sonare_engine_sample_at_ppq(engine_, ppq, &sample));
  return Napi::Number::New(env, static_cast<double>(sample));
}

Napi::Value RealtimeEngineWrap::SetLoop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const double start_ppq = info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 0.0;
  const double end_ppq = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.0;
  const bool enabled =
      info.Length() <= 2 || info[2].IsUndefined() ? true : info[2].As<Napi::Boolean>().Value();
  ThrowIfError(env, sonare_engine_set_loop(engine_, start_ppq, end_ppq, enabled ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::AddParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareParameterInfo parameter{};
  if (!ReadParameter(info, 0, &parameter)) return env.Undefined();
  ThrowIfError(env, sonare_engine_add_parameter(engine_, &parameter));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ParameterCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_parameter_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::ParameterInfoByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t index =
      info.Length() > 0 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value()) : 0;
  SonareParameterInfo parameter{};
  ThrowIfError(env, sonare_engine_parameter_info_by_index(engine_, index, &parameter));
  if (env.IsExceptionPending()) return env.Undefined();
  return ParameterToObject(env, parameter);
}

Napi::Value RealtimeEngineWrap::ParameterInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  SonareParameterInfo parameter{};
  ThrowIfError(env, sonare_engine_parameter_info(engine_, id, &parameter));
  if (env.IsExceptionPending()) return env.Undefined();
  return ParameterToObject(env, parameter);
}

Napi::Value RealtimeEngineWrap::SetAutomationLane(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 1 || !info[1].IsArray()) {
    Napi::TypeError::New(env, "expected an array of automation points")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint32_t param_id = info[0].As<Napi::Number>().Uint32Value();
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
    point.value = obj.Get("value").As<Napi::Number>().FloatValue();
    point.curve_to_next = obj.Has("curveToNext") && !obj.Get("curveToNext").IsUndefined()
                              ? obj.Get("curveToNext").As<Napi::Number>().Int32Value()
                              : 0;
    points.push_back(point);
  }
  ThrowIfError(env,
               sonare_engine_set_automation_lane(engine_, param_id, points.data(), points.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::AutomationLaneCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_automation_lane_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::SetMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of markers").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<SonareEngineMarker> markers;
  markers.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareEngineMarker marker{};
    marker.id = obj.Get("id").As<Napi::Number>().Uint32Value();
    marker.ppq = obj.Get("ppq").As<Napi::Number>().DoubleValue();
    CopyString(marker.name, sizeof(marker.name),
               obj.Has("name") && !obj.Get("name").IsUndefined()
                   ? obj.Get("name").As<Napi::String>().Utf8Value()
                   : "");
    markers.push_back(marker);
  }
  ThrowIfError(env, sonare_engine_set_markers(engine_, markers.data(), markers.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::MarkerCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_marker_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::MarkerByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t index =
      info.Length() > 0 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value()) : 0;
  SonareEngineMarker marker{};
  ThrowIfError(env, sonare_engine_marker_by_index(engine_, index, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  return MarkerToObject(env, marker);
}

Napi::Value RealtimeEngineWrap::Marker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  SonareEngineMarker marker{};
  ThrowIfError(env, sonare_engine_marker(engine_, id, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  return MarkerToObject(env, marker);
}

Napi::Value RealtimeEngineWrap::SeekMarker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  ThrowIfError(env, sonare_engine_seek_marker(engine_, id, OptionalInt64(info, 1, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetLoopFromMarkers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t start_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const uint32_t end_id = info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 0;
  ThrowIfError(env, sonare_engine_set_loop_from_markers(engine_, start_id, end_id));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMetronome(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a metronome config object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineMetronomeConfig config{};
  config.enabled = obj.Get("enabled").As<Napi::Boolean>().Value() ? 1 : 0;
  config.beat_gain = obj.Has("beatGain") && !obj.Get("beatGain").IsUndefined()
                         ? obj.Get("beatGain").As<Napi::Number>().FloatValue()
                         : 0.35f;
  config.accent_gain = obj.Has("accentGain") && !obj.Get("accentGain").IsUndefined()
                           ? obj.Get("accentGain").As<Napi::Number>().FloatValue()
                           : 0.7f;
  config.click_samples = obj.Has("clickSamples") && !obj.Get("clickSamples").IsUndefined()
                             ? obj.Get("clickSamples").As<Napi::Number>().Int32Value()
                             : 96;
  config.click_seconds = obj.Has("clickSeconds") && !obj.Get("clickSeconds").IsUndefined()
                             ? obj.Get("clickSeconds").As<Napi::Number>().DoubleValue()
                             : 0.0;
  ThrowIfError(env, sonare_engine_set_metronome(engine_, &config));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::Metronome(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareEngineMetronomeConfig config{};
  ThrowIfError(env, sonare_engine_metronome(engine_, &config));
  if (env.IsExceptionPending()) return env.Undefined();
  return MetronomeToObject(env, config);
}

Napi::Value RealtimeEngineWrap::CountInEndSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int64_t start_sample = OptionalInt64(info, 0, 0);
  const int bars = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 1;
  int64_t out = 0;
  ThrowIfError(env, sonare_engine_count_in_end_sample(engine_, start_sample, bars, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
}

Napi::Value RealtimeEngineWrap::SetParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t param_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const float value = info.Length() > 1 ? info[1].As<Napi::Number>().FloatValue() : 0.0f;
  ThrowIfError(env,
               sonare_engine_set_parameter(engine_, param_id, value, OptionalInt64(info, 2, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetParameterSmoothed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t param_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const float value = info.Length() > 1 ? info[1].As<Napi::Number>().FloatValue() : 0.0f;
  ThrowIfError(env, sonare_engine_set_parameter_smoothed(engine_, param_id, value,
                                                         OptionalInt64(info, 2, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetSoloMute(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t lane_index = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const bool solo = info.Length() > 1 && info[1].As<Napi::Boolean>().Value();
  const bool mute = info.Length() > 2 && info[2].As<Napi::Boolean>().Value();
  ThrowIfError(env, sonare_engine_set_solo_mute(engine_, lane_index, solo ? 1 : 0, mute ? 1 : 0,
                                                OptionalInt64(info, 3, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearParameters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_clear_parameters(engine_));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMidiClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
    clip.id = OptionalUint32(obj, "id", 0);
    clip.track_id = OptionalUint32(obj, "trackId", 0);
    clip.start_sample = OptionalInt64Property(obj, "startSample", 0);
    clip.start_ppq = OptionalDoubleProperty(obj, "startPpq", 0.0);
    clip.length_samples = OptionalInt64Property(obj, "lengthSamples", 0);
    clip.loop = obj.Has("loop") && obj.Get("loop").ToBoolean().Value() ? 1 : 0;
    clip.loop_length_samples = OptionalInt64Property(obj, "loopLengthSamples", 0);
    clip.destination_id = OptionalUint32(obj, "destinationId", OptionalUint32(obj, "trackId", 0));
    clip.events = event_storage[i].data();
    clip.event_count = event_storage[i].size();
    clips[i] = clip;
  }
  ThrowIfError(env, sonare_engine_set_midi_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetBuiltinInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  SonareEngineBuiltinSynthConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    if (!ReadEngineBuiltinSynthConfig(env, obj, &config)) return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_builtin_instrument(engine_, destination_id, &config));
  return env.Undefined();
}

// Binds the patch-driven NativeSynth to a realtime MIDI destination:
//   setSynthInstrument(destinationId, patch)
// where `patch` is a SynthPatch object or a preset-name string ("saw-lead" /
// "va:saw-lead"), resolving exactly like Project.bounceWithSynthInstruments.
Napi::Value RealtimeEngineWrap::SetSynthInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  SonareSynthPatch patch{};
  if (info.Length() > 1) {
    if (!sonare_node::ReadSynthPatch(env, info[1], &patch)) {
      return env.Undefined();  // exception already pending
    }
  } else {
    patch.struct_version = 1;
  }
  ThrowIfError(env, sonare_engine_set_synth_instrument(engine_, destination_id, &patch));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::LoadSoundFont(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
}

Napi::Value RealtimeEngineWrap::SetSf2Instrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  SonareEngineSf2InstrumentConfig config{};
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object obj = info[1].As<Napi::Object>();
    if (obj.Has("gain")) config.gain = obj.Get("gain").As<Napi::Number>().FloatValue();
    if (obj.Has("polyphony"))
      config.polyphony = obj.Get("polyphony").As<Napi::Number>().Int32Value();
  }
  ThrowIfError(env, sonare_engine_set_sf2_instrument(engine_, destination_id, &config));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearMidiInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  ThrowIfError(env, sonare_engine_clear_midi_instrument(engine_, destination_id));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::MidiInstrumentCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_instrument_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::PushMidiNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const uint8_t group =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t note =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t velocity =
      info.Length() > 4 ? static_cast<uint8_t>(info[4].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_note_on(engine_, destination_id, group, channel, note,
                                                    velocity, OptionalInt64(info, 5, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::BindMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t channel =
      info.Length() > 0 ? static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t controller =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint32_t param_id = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 0;
  const float min_value = info.Length() > 3 ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  const float max_value = info.Length() > 4 ? info[4].As<Napi::Number>().FloatValue() : 1.0f;
  ThrowIfError(env, sonare_engine_bind_midi_cc(engine_, channel, controller, param_id, min_value,
                                               max_value));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearMidiCcBindings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_clear_midi_cc_bindings(engine_));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::MidiCcBindingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_cc_binding_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::SetMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  std::string config = info.Length() > 1 && info[1].IsString()
                           ? info[1].As<Napi::String>().Utf8Value()
                           : std::string();
  ThrowIfError(env, sonare_engine_set_midi_fx(engine_, destination_id, config.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearMidiFx(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  ThrowIfError(env, sonare_engine_clear_midi_fx(engine_, destination_id));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  ThrowIfError(env, sonare_engine_set_midi_input_source(engine_, destination_id));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearMidiInputSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_clear_midi_input_source(engine_));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::MidiInputPendingCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_midi_input_pending_count(engine_, &count));
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t group =
      info.Length() > 0 ? static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t note =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t velocity =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_input_note_on(engine_, group, channel, note, velocity,
                                                          OptionalInt64(info, 4, 0)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PushMidiInputNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t group =
      info.Length() > 0 ? static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t note =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t velocity =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_input_note_off(engine_, group, channel, note, velocity,
                                                           OptionalInt64(info, 4, 0)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PushMidiInputCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint8_t group =
      info.Length() > 0 ? static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t controller =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t value =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_input_cc(engine_, group, channel, controller, value,
                                                     OptionalInt64(info, 4, 0)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PushMidiNoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const uint8_t group =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t note =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t velocity =
      info.Length() > 4 ? static_cast<uint8_t>(info[4].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_note_off(engine_, destination_id, group, channel, note,
                                                     velocity, OptionalInt64(info, 5, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PushMidiCc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t destination_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const uint8_t group =
      info.Length() > 1 ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t channel =
      info.Length() > 2 ? static_cast<uint8_t>(info[2].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t controller =
      info.Length() > 3 ? static_cast<uint8_t>(info[3].As<Napi::Number>().Uint32Value()) : 0;
  const uint8_t value =
      info.Length() > 4 ? static_cast<uint8_t>(info[4].As<Napi::Number>().Uint32Value()) : 0;
  ThrowIfError(env, sonare_engine_push_midi_cc(engine_, destination_id, group, channel, controller,
                                               value, OptionalInt64(info, 5, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PushMidiPanic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_push_midi_panic(engine_, OptionalInt64(info, 0, -1)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::GetTransportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
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
  return out;
}

void RealtimeEngineWrap::Destroy(const Napi::CallbackInfo& info) {
  (void)info;
  if (engine_ != nullptr) {
    sonare_engine_destroy(engine_);
    engine_ = nullptr;
  }
}
