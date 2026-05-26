#include "sonare_wrap_engine.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap_utils.h"

namespace {

// Must match sonare::rt::kEngineAbiVersion (src/rt/command.h) and the WASM
// binding's EXPECTED_ENGINE_ABI_VERSION. A mismatch means the loaded native
// binary lays out engine structs differently than this addon expects.
constexpr uint32_t kExpectedEngineAbiVersion = 2;

void ThrowIfError(Napi::Env env, SonareError err) {
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_node::ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  }
}

int64_t OptionalInt64(const Napi::CallbackInfo& info, size_t index, int64_t fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return static_cast<int64_t>(info[index].As<Napi::Number>().Int64Value());
}

void CopyString(char* dest, size_t capacity, const std::string& value) {
  if (capacity == 0) return;
  std::strncpy(dest, value.c_str(), capacity - 1);
  dest[capacity - 1] = '\0';
}

Napi::Object ParameterToObject(Napi::Env env, const SonareParameterInfo& parameter) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, parameter.id));
  out.Set("name", Napi::String::New(env, parameter.name));
  out.Set("unit", Napi::String::New(env, parameter.unit));
  out.Set("minValue", Napi::Number::New(env, parameter.min_value));
  out.Set("maxValue", Napi::Number::New(env, parameter.max_value));
  out.Set("defaultValue", Napi::Number::New(env, parameter.default_value));
  out.Set("rtSafe", Napi::Boolean::New(env, parameter.rt_safe != 0));
  out.Set("defaultCurve", Napi::Number::New(env, parameter.default_curve));
  return out;
}

Napi::Object MarkerToObject(Napi::Env env, const SonareEngineMarker& marker) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, marker.id));
  out.Set("ppq", Napi::Number::New(env, marker.ppq));
  out.Set("name", Napi::String::New(env, marker.name));
  return out;
}

Napi::Object MetronomeToObject(Napi::Env env, const SonareEngineMetronomeConfig& config) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("enabled", Napi::Boolean::New(env, config.enabled != 0));
  out.Set("beatGain", Napi::Number::New(env, config.beat_gain));
  out.Set("accentGain", Napi::Number::New(env, config.accent_gain));
  out.Set("clickSamples", Napi::Number::New(env, config.click_samples));
  return out;
}

bool ReadParameter(const Napi::CallbackInfo& info, size_t index, SonareParameterInfo* out) {
  Napi::Env env = info.Env();
  if (info.Length() <= index || !info[index].IsObject()) {
    Napi::TypeError::New(env, "expected a parameter info object").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Object obj = info[index].As<Napi::Object>();
  std::memset(out, 0, sizeof(*out));
  out->id = obj.Get("id").As<Napi::Number>().Uint32Value();
  CopyString(out->name, sizeof(out->name), obj.Get("name").As<Napi::String>().Utf8Value());
  CopyString(out->unit, sizeof(out->unit), obj.Get("unit").As<Napi::String>().Utf8Value());
  out->min_value = obj.Get("minValue").As<Napi::Number>().FloatValue();
  out->max_value = obj.Get("maxValue").As<Napi::Number>().FloatValue();
  out->default_value = obj.Get("defaultValue").As<Napi::Number>().FloatValue();
  out->rt_safe = obj.Get("rtSafe").As<Napi::Boolean>().Value() ? 1 : 0;
  out->default_curve = obj.Get("defaultCurve").As<Napi::Number>().Int32Value();
  return true;
}

Napi::Object TelemetryToObject(Napi::Env env, const SonareEngineTelemetry& telemetry) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::Number::New(env, telemetry.type));
  out.Set("error", Napi::Number::New(env, telemetry.error));
  out.Set("renderFrame", Napi::Number::New(env, static_cast<double>(telemetry.render_frame)));
  out.Set("timelineSample", Napi::Number::New(env, static_cast<double>(telemetry.timeline_sample)));
  out.Set("audibleTimelineSample",
          Napi::Number::New(env, static_cast<double>(telemetry.audible_timeline_sample)));
  out.Set("graphLatencySamplesQ8", Napi::Number::New(env, telemetry.graph_latency_samples_q8));
  out.Set("value", Napi::Number::New(env, telemetry.value));
  return out;
}

Napi::Object MeterTelemetryToObject(Napi::Env env, const SonareMeterTelemetryRecord& record) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("targetId", Napi::Number::New(env, record.target_id));
  out.Set("renderFrame", Napi::Number::New(env, static_cast<double>(record.render_frame)));
  out.Set("seq", Napi::Number::New(env, static_cast<double>(record.seq)));
  Napi::Array peak_db = Napi::Array::New(env, 2);
  peak_db.Set(0u, Napi::Number::New(env, record.peak_db_l));
  peak_db.Set(1u, Napi::Number::New(env, record.peak_db_r));
  out.Set("peakDb", peak_db);
  Napi::Array rms_db = Napi::Array::New(env, 2);
  rms_db.Set(0u, Napi::Number::New(env, record.rms_db_l));
  rms_db.Set(1u, Napi::Number::New(env, record.rms_db_r));
  out.Set("rmsDb", rms_db);
  Napi::Array true_peak_db = Napi::Array::New(env, 2);
  true_peak_db.Set(0u, Napi::Number::New(env, record.true_peak_db_l));
  true_peak_db.Set(1u, Napi::Number::New(env, record.true_peak_db_r));
  out.Set("truePeakDb", true_peak_db);
  out.Set("maxTruePeakDb", Napi::Number::New(env, record.max_true_peak_db));
  out.Set("correlation", Napi::Number::New(env, record.correlation));
  out.Set("monoCompatWidth", Napi::Number::New(env, record.mono_compat_width));
  out.Set("momentaryLufs", Napi::Number::New(env, record.momentary_lufs));
  out.Set("shortTermLufs", Napi::Number::New(env, record.short_term_lufs));
  out.Set("integratedLufs", Napi::Number::New(env, record.integrated_lufs));
  out.Set("gainReductionDb", Napi::Number::New(env, record.gain_reduction_db));
  out.Set("droppedRecords", Napi::Number::New(env, record.dropped_records));
  return out;
}

struct ChannelBlock {
  std::vector<std::vector<float>> storage;
  std::vector<float*> pointers;
  int frames = 0;
};

ChannelBlock ReadChannels(const Napi::CallbackInfo& info, size_t index) {
  Napi::Env env = info.Env();
  if (info.Length() <= index || !info[index].IsArray()) {
    Napi::TypeError::New(env, "expected an array of Float32Array channels")
        .ThrowAsJavaScriptException();
    return {};
  }

  Napi::Array channels = info[index].As<Napi::Array>();
  const uint32_t count = channels.Length();
  if (count == 0) {
    Napi::TypeError::New(env, "channels must not be empty").ThrowAsJavaScriptException();
    return {};
  }

  ChannelBlock block;
  block.storage.reserve(count);
  block.pointers.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value value = channels.Get(i);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "each channel must be a Float32Array").ThrowAsJavaScriptException();
      return {};
    }
    Napi::Float32Array channel = value.As<Napi::Float32Array>();
    if (i == 0) {
      block.frames = static_cast<int>(channel.ElementLength());
      if (block.frames <= 0) {
        Napi::TypeError::New(env, "channels must not be empty").ThrowAsJavaScriptException();
        return {};
      }
    } else if (static_cast<int>(channel.ElementLength()) != block.frames) {
      Napi::TypeError::New(env, "all channels must have the same length")
          .ThrowAsJavaScriptException();
      return {};
    }
    block.storage.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
  }

  for (auto& channel : block.storage) {
    block.pointers.push_back(channel.data());
  }
  return block;
}

Napi::Array ChannelsToJs(Napi::Env env, const ChannelBlock& block) {
  Napi::Array out = Napi::Array::New(env, block.storage.size());
  for (size_t ch = 0; ch < block.storage.size(); ++ch) {
    auto array = Napi::Float32Array::New(env, block.storage[ch].size());
    std::memcpy(array.Data(), block.storage[ch].data(), block.storage[ch].size() * sizeof(float));
    out.Set(static_cast<uint32_t>(ch), array);
  }
  return out;
}

int IntProperty(const Napi::Object& obj, const char* key, int fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().Int32Value();
}

int64_t Int64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback
                             : static_cast<int64_t>(value.As<Napi::Number>().Int64Value());
}

float FloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().FloatValue();
}

bool BoolProperty(const Napi::Object& obj, const char* key, bool fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Boolean>().Value();
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
          InstanceMethod<&RealtimeEngineWrap::SetCaptureBuffer>("setCaptureBuffer"),
          InstanceMethod<&RealtimeEngineWrap::ArmCapture>("armCapture"),
          InstanceMethod<&RealtimeEngineWrap::SetCapturePunch>("setCapturePunch"),
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
                              : 1;
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

Napi::Value RealtimeEngineWrap::SetClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of clips").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<std::vector<std::vector<float>>> storage;
  std::vector<std::vector<const float*>> ptr_storage;
  std::vector<SonareEngineClip> clips;
  storage.reserve(input.Length());
  ptr_storage.reserve(input.Length());
  clips.reserve(input.Length());

  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    Napi::Array channels = obj.Get("channels").As<Napi::Array>();
    if (channels.Length() == 0) {
      Napi::TypeError::New(env, "clip channels must not be empty").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    storage.emplace_back();
    ptr_storage.emplace_back();
    auto& clip_storage = storage.back();
    auto& clip_ptrs = ptr_storage.back();
    clip_storage.reserve(channels.Length());
    clip_ptrs.reserve(channels.Length());
    size_t num_samples = 0;
    for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
      Napi::Value value = channels.Get(ch);
      if (!sonare_node::IsFloat32Array(value)) {
        Napi::TypeError::New(env, "clip channel must be a Float32Array")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Float32Array channel = value.As<Napi::Float32Array>();
      if (ch == 0) {
        num_samples = channel.ElementLength();
      } else if (channel.ElementLength() != num_samples) {
        Napi::TypeError::New(env, "all clip channels must have the same length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      clip_storage.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
      clip_ptrs.push_back(clip_storage.back().data());
    }

    SonareEngineClip clip{};
    clip.id = obj.Get("id").As<Napi::Number>().Uint32Value();
    clip.channels = clip_ptrs.data();
    clip.num_channels = static_cast<int>(clip_ptrs.size());
    clip.num_samples = static_cast<int64_t>(num_samples);
    clip.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
    clip.clip_offset_samples =
        obj.Has("clipOffsetSamples") && !obj.Get("clipOffsetSamples").IsUndefined()
            ? obj.Get("clipOffsetSamples").As<Napi::Number>().Int64Value()
            : 0;
    clip.length_samples = obj.Has("lengthSamples") && !obj.Get("lengthSamples").IsUndefined()
                              ? obj.Get("lengthSamples").As<Napi::Number>().Int64Value()
                              : static_cast<int64_t>(num_samples);
    clip.loop = obj.Has("loop") && !obj.Get("loop").IsUndefined()
                    ? (obj.Get("loop").As<Napi::Boolean>().Value() ? 1 : 0)
                    : 0;
    clip.gain = obj.Has("gain") && !obj.Get("gain").IsUndefined()
                    ? obj.Get("gain").As<Napi::Number>().FloatValue()
                    : 1.0f;
    clip.fade_in_samples = obj.Has("fadeInSamples") && !obj.Get("fadeInSamples").IsUndefined()
                               ? obj.Get("fadeInSamples").As<Napi::Number>().Int64Value()
                               : 0;
    clip.fade_out_samples = obj.Has("fadeOutSamples") && !obj.Get("fadeOutSamples").IsUndefined()
                                ? obj.Get("fadeOutSamples").As<Napi::Number>().Int64Value()
                                : 0;
    clips.push_back(clip);
  }

  ThrowIfError(env, sonare_engine_set_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_clip_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::SetCaptureBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of Float32Array channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array channels = info[0].As<Napi::Array>();
  if (channels.Length() == 0) {
    Napi::TypeError::New(env, "capture channels must not be empty").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Reference<Napi::Float32Array>> refs;
  std::vector<float*> ptrs;
  refs.reserve(channels.Length());
  ptrs.reserve(channels.Length());
  int64_t frames = 0;
  for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
    Napi::Value value = channels.Get(ch);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "capture channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Float32Array channel = value.As<Napi::Float32Array>();
    if (ch == 0) {
      frames = static_cast<int64_t>(channel.ElementLength());
      if (frames <= 0) {
        Napi::TypeError::New(env, "capture channels must not be empty")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
    } else if (static_cast<int64_t>(channel.ElementLength()) != frames) {
      Napi::TypeError::New(env, "all capture channels must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    refs.push_back(Napi::Persistent(channel));
    ptrs.push_back(channel.Data());
  }

  SonareEngineCaptureBuffer buffer{};
  buffer.channels = ptrs.data();
  buffer.num_channels = static_cast<int>(ptrs.size());
  buffer.capacity_frames = frames;
  ThrowIfError(env, sonare_engine_set_capture_buffer(engine_, &buffer));
  if (env.IsExceptionPending()) return env.Undefined();
  capture_refs_ = std::move(refs);
  capture_ptrs_ = std::move(ptrs);
  capture_capacity_frames_ = frames;
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ArmCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const bool armed =
      info.Length() <= 0 || info[0].IsUndefined() ? true : info[0].As<Napi::Boolean>().Value();
  ThrowIfError(env, sonare_engine_arm_capture(engine_, armed ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetCapturePunch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int64_t start_sample = OptionalInt64(info, 0, 0);
  const int64_t end_sample = OptionalInt64(info, 1, 0);
  const bool enabled =
      info.Length() <= 2 || info[2].IsUndefined() ? true : info[2].As<Napi::Boolean>().Value();
  ThrowIfError(env,
               sonare_engine_set_capture_punch(engine_, start_sample, end_sample, enabled ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ResetCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_reset_capture(engine_));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::CaptureStatus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareEngineCaptureStatus status{};
  ThrowIfError(env, sonare_engine_capture_status(engine_, &status));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("capturedFrames", Napi::Number::New(env, static_cast<double>(status.captured_frames)));
  out.Set("overflowCount", Napi::Number::New(env, status.overflow_count));
  out.Set("armed", Napi::Boolean::New(env, status.armed != 0));
  out.Set("punchEnabled", Napi::Boolean::New(env, status.punch_enabled != 0));
  return out;
}

Napi::Value RealtimeEngineWrap::CapturedAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (engine_ == nullptr) {
    Napi::Error::New(env, "RealtimeEngine is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareEngineCaptureStatus status{};
  ThrowIfError(env, sonare_engine_capture_status(engine_, &status));
  if (env.IsExceptionPending()) return env.Undefined();

  // Clamp the captured frame count to the JS-supplied buffer capacity so that we
  // never read past the Float32Arrays handed to setCaptureBuffer().
  int64_t frames = status.captured_frames;
  if (frames < 0) frames = 0;
  if (frames > capture_capacity_frames_) frames = capture_capacity_frames_;

  Napi::Array out = Napi::Array::New(env, capture_refs_.size());
  for (size_t ch = 0; ch < capture_refs_.size(); ++ch) {
    Napi::Float32Array source = capture_refs_[ch].Value();
    const size_t count = static_cast<size_t>(frames);
    auto channel = Napi::Float32Array::New(env, count);
    if (count > 0) {
      std::memcpy(channel.Data(), source.Data(), count * sizeof(float));
    }
    out.Set(static_cast<uint32_t>(ch), channel);
  }
  return out;
}

Napi::Value RealtimeEngineWrap::SetGraph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a graph spec object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object spec_obj = info[0].As<Napi::Object>();
  Napi::Array node_input = spec_obj.Get("nodes").As<Napi::Array>();
  Napi::Array connection_input = spec_obj.Get("connections").As<Napi::Array>();

  std::vector<SonareEngineGraphNode> nodes;
  nodes.reserve(node_input.Length());
  for (uint32_t i = 0; i < node_input.Length(); ++i) {
    Napi::Object obj = node_input.Get(i).As<Napi::Object>();
    SonareEngineGraphNode node{};
    CopyString(node.id, sizeof(node.id), obj.Get("id").As<Napi::String>().Utf8Value());
    node.type = obj.Has("type") && !obj.Get("type").IsUndefined()
                    ? obj.Get("type").As<Napi::Number>().Int32Value()
                    : 0;
    node.gain_db = obj.Has("gainDb") && !obj.Get("gainDb").IsUndefined()
                       ? obj.Get("gainDb").As<Napi::Number>().FloatValue()
                       : 0.0f;
    node.num_ports = obj.Has("numPorts") && !obj.Get("numPorts").IsUndefined()
                         ? obj.Get("numPorts").As<Napi::Number>().Int32Value()
                         : 0;
    nodes.push_back(node);
  }

  std::vector<SonareEngineGraphConnection> connections;
  connections.reserve(connection_input.Length());
  for (uint32_t i = 0; i < connection_input.Length(); ++i) {
    Napi::Object obj = connection_input.Get(i).As<Napi::Object>();
    SonareEngineGraphConnection connection{};
    CopyString(connection.source_node, sizeof(connection.source_node),
               obj.Get("sourceNode").As<Napi::String>().Utf8Value());
    connection.source_port = obj.Get("sourcePort").As<Napi::Number>().Int32Value();
    CopyString(connection.dest_node, sizeof(connection.dest_node),
               obj.Get("destNode").As<Napi::String>().Utf8Value());
    connection.dest_port = obj.Get("destPort").As<Napi::Number>().Int32Value();
    connection.mix = obj.Has("mix") && !obj.Get("mix").IsUndefined()
                         ? obj.Get("mix").As<Napi::Number>().Int32Value()
                         : 1;
    connections.push_back(connection);
  }

  std::vector<SonareEngineGraphParameterBinding> parameter_bindings;
  if (spec_obj.Has("parameterBindings") && !spec_obj.Get("parameterBindings").IsUndefined()) {
    Napi::Array binding_input = spec_obj.Get("parameterBindings").As<Napi::Array>();
    parameter_bindings.reserve(binding_input.Length());
    for (uint32_t i = 0; i < binding_input.Length(); ++i) {
      Napi::Object obj = binding_input.Get(i).As<Napi::Object>();
      SonareEngineGraphParameterBinding binding{};
      binding.param_id = obj.Get("paramId").As<Napi::Number>().Uint32Value();
      CopyString(binding.node_id, sizeof(binding.node_id),
                 obj.Get("nodeId").As<Napi::String>().Utf8Value());
      parameter_bindings.push_back(binding);
    }
  }

  SonareEngineGraphSpec spec{};
  spec.nodes = nodes.data();
  spec.node_count = nodes.size();
  spec.connections = connections.data();
  spec.connection_count = connections.size();
  spec.parameter_bindings = parameter_bindings.data();
  spec.parameter_binding_count = parameter_bindings.size();
  CopyString(spec.input_node, sizeof(spec.input_node),
             spec_obj.Get("inputNode").As<Napi::String>().Utf8Value());
  CopyString(spec.output_node, sizeof(spec.output_node),
             spec_obj.Get("outputNode").As<Napi::String>().Utf8Value());
  spec.num_channels = spec_obj.Has("numChannels") && !spec_obj.Get("numChannels").IsUndefined()
                          ? spec_obj.Get("numChannels").As<Napi::Number>().Int32Value()
                          : 2;
  ThrowIfError(env, sonare_engine_set_graph(engine_, &spec));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::GraphNodeCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_graph_node_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::GraphConnectionCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_graph_connection_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::Process(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_process(engine_, block.pointers.data(),
                                          static_cast<int>(block.pointers.size()), block.frames));
  if (env.IsExceptionPending()) return env.Undefined();
  return ChannelsToJs(env, block);
}

Napi::Value RealtimeEngineWrap::ProcessWithMonitor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();

  ChannelBlock monitor;
  monitor.frames = block.frames;
  monitor.storage.resize(block.storage.size());
  monitor.pointers.reserve(block.storage.size());
  for (size_t ch = 0; ch < block.storage.size(); ++ch) {
    monitor.storage[ch].assign(static_cast<size_t>(block.frames), 0.0f);
    monitor.pointers.push_back(monitor.storage[ch].data());
  }

  ThrowIfError(env, sonare_engine_process_with_monitor(
                        engine_, block.pointers.data(), monitor.pointers.data(),
                        static_cast<int>(block.pointers.size()), block.frames));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Object result = Napi::Object::New(env);
  result.Set("output", ChannelsToJs(env, block));
  result.Set("monitor", ChannelsToJs(env, monitor));
  return result;
}

Napi::Value RealtimeEngineWrap::RenderOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();
  const int block_size =
      info.Length() > 1 && !info[1].IsUndefined() ? info[1].As<Napi::Number>().Int32Value() : 128;
  ThrowIfError(env, sonare_engine_render_offline(engine_, block.pointers.data(),
                                                 static_cast<int>(block.pointers.size()),
                                                 block.frames, block_size));
  if (env.IsExceptionPending()) return env.Undefined();
  return ChannelsToJs(env, block);
}

Napi::Value RealtimeEngineWrap::BounceOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a bounce options object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineBounceOptions options{};
  options.total_frames = Int64Property(obj, "totalFrames", 0);
  options.block_size = IntProperty(obj, "blockSize", 128);
  options.num_channels = IntProperty(obj, "numChannels", 2);
  options.target_sample_rate = IntProperty(obj, "targetSampleRate", 48000);
  options.source_sample_rate = IntProperty(obj, "sourceSampleRate", 48000);
  options.normalize_lufs = BoolProperty(obj, "normalizeLufs", false) ? 1 : 0;
  options.target_lufs = FloatProperty(obj, "targetLufs", -14.0f);
  options.dither = IntProperty(obj, "dither", 0);
  options.dither_bits = IntProperty(obj, "ditherBits", 16);
  options.dither_seed = static_cast<uint32_t>(Int64Property(obj, "ditherSeed", 0));
  SonareEngineBounceResult result{};
  ThrowIfError(env, sonare_engine_bounce_offline(engine_, &options, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array interleaved = Napi::Float32Array::New(env, result.sample_count);
  if (result.sample_count > 0 && result.interleaved != nullptr) {
    std::memcpy(interleaved.Data(), result.interleaved, result.sample_count * sizeof(float));
  }
  sonare_free_bounce_result(&result);
  Napi::Object out = Napi::Object::New(env);
  out.Set("interleaved", interleaved);
  out.Set("frames", Napi::Number::New(env, static_cast<double>(result.frames)));
  out.Set("numChannels", Napi::Number::New(env, result.num_channels));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("integratedLufs", Napi::Number::New(env, result.integrated_lufs));
  return out;
}

Napi::Value RealtimeEngineWrap::FreezeOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a freeze options object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineFreezeOptions options{};
  options.total_frames = Int64Property(obj, "totalFrames", 0);
  options.block_size = IntProperty(obj, "blockSize", 128);
  options.num_channels = IntProperty(obj, "numChannels", 2);
  options.clip_id = static_cast<uint32_t>(Int64Property(obj, "clipId", 1));
  options.start_ppq = FloatProperty(obj, "startPpq", 0.0f);
  options.gain = FloatProperty(obj, "gain", 1.0f);
  SonareEngineFreezeResult result{};
  ThrowIfError(env, sonare_engine_freeze_offline(engine_, &options, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, result.clip_id));
  out.Set("frames", Napi::Number::New(env, static_cast<double>(result.frames)));
  out.Set("numChannels", Napi::Number::New(env, result.num_channels));
  return out;
}

Napi::Value RealtimeEngineWrap::DrainTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareEngineTelemetry> records(max_records);
  size_t written = 0;
  ThrowIfError(env,
               sonare_engine_drain_telemetry(engine_, records.data(), records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), TelemetryToObject(env, records[i]));
  }
  return out;
}

Napi::Value RealtimeEngineWrap::DrainMeterTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareMeterTelemetryRecord> records(max_records);
  size_t written = 0;
  ThrowIfError(
      env, sonare_engine_drain_meter_telemetry(engine_, records.data(), records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), MeterTelemetryToObject(env, records[i]));
  }
  return out;
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

Napi::Value RealtimeEngineWrap::GetTransportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareTransportState state{};
  ThrowIfError(env, sonare_engine_get_transport_state(engine_, &state));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("isPlaying", Napi::Boolean::New(env, state.playing != 0));
  out.Set("looping", Napi::Boolean::New(env, state.looping != 0));
  out.Set("renderFrame", Napi::Number::New(env, static_cast<double>(state.render_frame)));
  out.Set("samplePosition", Napi::Number::New(env, static_cast<double>(state.sample_position)));
  out.Set("ppq", Napi::Number::New(env, state.ppq_position));
  out.Set("bpm", Napi::Number::New(env, state.bpm));
  out.Set("loopStartPpq", Napi::Number::New(env, state.loop_start_ppq));
  out.Set("loopEndPpq", Napi::Number::New(env, state.loop_end_ppq));
  out.Set("sampleRate", Napi::Number::New(env, state.sample_rate));
  return out;
}

void RealtimeEngineWrap::Destroy(const Napi::CallbackInfo& info) {
  (void)info;
  if (engine_ != nullptr) {
    sonare_engine_destroy(engine_);
    engine_ = nullptr;
  }
}
