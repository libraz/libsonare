#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap_engine.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::engine {

constexpr uint32_t kExpectedEngineAbiVersion = 3;

inline void ThrowIfError(Napi::Env env, SonareError err) {
  if (err != SONARE_OK) {
    Napi::Error::New(env, sonare_node::ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  }
}

inline int64_t OptionalInt64(const Napi::CallbackInfo& info, size_t index, int64_t fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return static_cast<int64_t>(info[index].As<Napi::Number>().Int64Value());
}

inline void CopyString(char* dest, size_t capacity, const std::string& value) {
  if (capacity == 0) return;
  std::strncpy(dest, value.c_str(), capacity - 1);
  dest[capacity - 1] = '\0';
}

inline Napi::Object ParameterToObject(Napi::Env env, const SonareParameterInfo& parameter) {
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

inline Napi::Object MarkerToObject(Napi::Env env, const SonareEngineMarker& marker) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, marker.id));
  out.Set("ppq", Napi::Number::New(env, marker.ppq));
  out.Set("name", Napi::String::New(env, marker.name));
  return out;
}

inline Napi::Object MetronomeToObject(Napi::Env env, const SonareEngineMetronomeConfig& config) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("enabled", Napi::Boolean::New(env, config.enabled != 0));
  out.Set("beatGain", Napi::Number::New(env, config.beat_gain));
  out.Set("accentGain", Napi::Number::New(env, config.accent_gain));
  out.Set("clickSamples", Napi::Number::New(env, config.click_samples));
  out.Set("clickSeconds", Napi::Number::New(env, config.click_seconds));
  return out;
}

inline bool ReadParameter(const Napi::CallbackInfo& info, size_t index, SonareParameterInfo* out) {
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

inline Napi::Object TelemetryToObject(Napi::Env env, const SonareEngineTelemetry& telemetry) {
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

inline Napi::Object MeterTelemetryToObject(Napi::Env env,
                                           const SonareMeterTelemetryRecord& record) {
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

inline ChannelBlock ReadChannels(const Napi::CallbackInfo& info, size_t index) {
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

inline Napi::Array ChannelsToJs(Napi::Env env, const ChannelBlock& block) {
  Napi::Array out = Napi::Array::New(env, block.storage.size());
  for (size_t ch = 0; ch < block.storage.size(); ++ch) {
    auto array = Napi::Float32Array::New(env, block.storage[ch].size());
    std::memcpy(array.Data(), block.storage[ch].data(), block.storage[ch].size() * sizeof(float));
    out.Set(static_cast<uint32_t>(ch), array);
  }
  return out;
}

inline int IntProperty(const Napi::Object& obj, const char* key, int fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().Int32Value();
}

inline int64_t Int64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback
                             : static_cast<int64_t>(value.As<Napi::Number>().Int64Value());
}

inline float FloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Number>().FloatValue();
}

inline bool BoolProperty(const Napi::Object& obj, const char* key, bool fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() ? fallback : value.As<Napi::Boolean>().Value();
}

}  // namespace sonare_node::engine
