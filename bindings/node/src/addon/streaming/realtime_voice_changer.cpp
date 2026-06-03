#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

std::string JsonTextFromJs(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return "neutral-monitor";
  if (value.IsString()) return value.As<Napi::String>().Utf8Value();
  Napi::Env env = value.Env();
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function stringify = json.Get("stringify").As<Napi::Function>();
  return stringify.Call(json, {value}).As<Napi::String>().Utf8Value();
}

}  // namespace

Napi::FunctionReference RealtimeVoiceChangerWrap::constructor_;

Napi::Object RealtimeVoiceChangerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "RealtimeVoiceChanger",
      {
          InstanceMethod<&RealtimeVoiceChangerWrap::Prepare>("prepare"),
          InstanceMethod<&RealtimeVoiceChangerWrap::Reset>("reset"),
          InstanceMethod<&RealtimeVoiceChangerWrap::SetConfig>("setConfig"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ConfigJson>("configJson"),
          InstanceMethod<&RealtimeVoiceChangerWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMono>("processMono"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMonoInto>("processMonoInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleaved>("processInterleaved"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleavedInto>(
              "processInterleavedInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessPlanarStereo>("processPlanarStereo"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("RealtimeVoiceChanger", func);
  return exports;
}

RealtimeVoiceChangerWrap::RealtimeVoiceChangerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RealtimeVoiceChangerWrap>(info) {
  Napi::Env env = info.Env();
  try {
    const auto text = info.Length() >= 1 ? JsonTextFromJs(info[0]) : std::string("neutral-monitor");
    changer_.set_config(
        sonare::editing::voice_changer::realtime_voice_changer_config_from_json(text));
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
  }
}

RealtimeVoiceChangerWrap::~RealtimeVoiceChangerWrap() = default;

Napi::Value RealtimeVoiceChangerWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (sampleRate, maxBlockSize?, channels?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const double sample_rate = info[0].As<Napi::Number>().DoubleValue();
  const int max_block_size =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 128;
  const int channels =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 1;
  changer_.prepare(sample_rate, max_block_size, channels);
  prepared_ = true;
  max_block_size_ = max_block_size;
  channels_ = channels;
  // RT-safe planar scratch + pointer table. Allocated once per prepare(); the
  // process methods only deinterleave into this buffer without further
  // allocations.
  planar_scratch_.assign(static_cast<size_t>(channels_) * static_cast<size_t>(max_block_size_),
                         0.0f);
  planar_ptrs_.assign(static_cast<size_t>(channels_), nullptr);
  for (int ch = 0; ch < channels_; ++ch) {
    planar_ptrs_[static_cast<size_t>(ch)] =
        planar_scratch_.data() + static_cast<size_t>(ch) * static_cast<size_t>(max_block_size_);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  changer_.reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected (presetIdOrConfig)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  changer_.set_config(sonare::editing::voice_changer::realtime_voice_changer_config_from_json(
      JsonTextFromJs(info[0])));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ConfigJson(const Napi::CallbackInfo& info) {
  return Napi::String::New(
      info.Env(),
      sonare::editing::voice_changer::realtime_voice_changer_config_to_json(changer_.config()));
}

Napi::Value RealtimeVoiceChangerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), changer_.latency_samples());
}

Napi::Value RealtimeVoiceChangerWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!prepared_) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared before processing")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  if (input.ElementLength() > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array output = Napi::Float32Array::New(env, input.ElementLength());
  changer_.process_block(input.Data(), output.Data(), static_cast<int>(input.ElementLength()));
  return output;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessMonoInto(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!prepared_) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared before processing")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (inputFloat32Array, outputFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  Napi::Float32Array output = info[1].As<Napi::Float32Array>();
  if (input.ElementLength() != output.ElementLength()) {
    Napi::RangeError::New(env, "input and output lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (input.ElementLength() > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  changer_.process_block(input.Data(), output.Data(), static_cast<int>(input.ElementLength()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!prepared_) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared before processing")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (interleavedFloat32Array, channels)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  const int channels = info[1].As<Napi::Number>().Int32Value();
  if (channels < 1 || channels > channels_ ||
      input.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(channels);
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // RT-safe path: reuse the per-instance planar scratch (allocated in
  // Prepare) and pointer table instead of allocating per call.
  const size_t stride = static_cast<size_t>(max_block_size_);
  for (int ch = 0; ch < channels; ++ch) {
    float* dst = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      dst[i] = input.Data()[i * static_cast<size_t>(channels) + ch];
    }
  }
  changer_.process_block(planar_ptrs_.data(), channels, static_cast<int>(frames));
  Napi::Float32Array output = Napi::Float32Array::New(env, input.ElementLength());
  for (int ch = 0; ch < channels; ++ch) {
    const float* src = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      output.Data()[i * static_cast<size_t>(channels) + ch] = src[i];
    }
  }
  return output;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedInto(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!prepared_) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared before processing")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2])) {
    Napi::TypeError::New(
        env, "Expected (inputInterleavedFloat32Array, channels, outputInterleavedFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  const int channels = info[1].As<Napi::Number>().Int32Value();
  Napi::Float32Array output = info[2].As<Napi::Float32Array>();
  if (input.ElementLength() != output.ElementLength()) {
    Napi::RangeError::New(env, "input and output lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels < 1 || channels > channels_ ||
      input.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(channels);
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // RT-safe path: reuse the per-instance planar scratch (allocated in
  // Prepare) and pointer table. Deinterleave -> process_block -> interleave
  // back into the caller-supplied output buffer; no per-call allocation.
  const size_t stride = static_cast<size_t>(max_block_size_);
  const float* in_data = input.Data();
  for (int ch = 0; ch < channels; ++ch) {
    float* dst = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      dst[i] = in_data[i * static_cast<size_t>(channels) + ch];
    }
  }
  changer_.process_block(planar_ptrs_.data(), channels, static_cast<int>(frames));
  float* out_data = output.Data();
  for (int ch = 0; ch < channels; ++ch) {
    const float* src = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      out_data[i * static_cast<size_t>(channels) + ch] = src[i];
    }
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!prepared_) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared before processing")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels_ < 2) {
    Napi::Error::New(env, "RealtimeVoiceChanger must be prepared with at least 2 channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = left.ElementLength();
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // In-place planar stereo: the planar process_block reads and writes the
  // supplied channel buffers directly, so the caller's L/R arrays are mutated.
  float* channels[2] = {left.Data(), right.Data()};
  changer_.process_block(channels, 2, static_cast<int>(frames));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Array out = Napi::Array::New(env);
  const auto names = sonare::editing::voice_changer::realtime_voice_changer_preset_names();
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), names[i]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (presetId)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto preset = sonare::editing::voice_changer::realtime_voice_changer_preset_from_id(
      info[0].As<Napi::String>().Utf8Value());
  return Napi::String::New(
      env, sonare::editing::voice_changer::realtime_voice_changer_preset_json(preset));
  SONARE_NODE_CATCH(env)
}

Napi::Value ValidateRealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (json)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string normalized;
  std::string error;
  const bool ok = sonare::editing::voice_changer::validate_realtime_voice_changer_preset_json(
      info[0].As<Napi::String>().Utf8Value(), &normalized, &error);
  Napi::Object out = Napi::Object::New(env);
  out.Set("ok", ok);
  if (ok) {
    out.Set("normalizedJson", normalized);
  } else {
    out.Set("error", error);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
