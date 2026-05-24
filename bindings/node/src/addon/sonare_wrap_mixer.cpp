#include "sonare_wrap_mixer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_utils.h"

namespace sonare_node {

Napi::FunctionReference MixerWrap::constructor_;

Napi::Object MixerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Mixer",
      {
          InstanceMethod<&MixerWrap::Compile>("compile"),
          InstanceMethod<&MixerWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&MixerWrap::StripCount>("stripCount"),
          InstanceMethod<&MixerWrap::ScheduleInsertAutomation>("scheduleInsertAutomation"),
          InstanceMethod<&MixerWrap::ToSceneJson>("toSceneJson"),
          InstanceMethod<&MixerWrap::Destroy>("destroy"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("Mixer", func);
  return exports;
}

MixerWrap::MixerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MixerWrap>(info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (sceneJson, sampleRate?, blockSize?)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string json = info[0].As<Napi::String>().Utf8Value();
  sample_rate_ =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 48000;
  block_size_ =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;

  mixer_ = sonare_mixer_from_scene_json(json.c_str(), sample_rate_, block_size_);
  if (mixer_ == nullptr) {
    Napi::Error::New(
        env, std::string("failed to build mixer from scene JSON: ") + sonare_last_error_message())
        .ThrowAsJavaScriptException();
    return;
  }
}

MixerWrap::~MixerWrap() {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

Napi::Value MixerWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_mixer_compile(mixer_);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to compile mixer graph: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (leftChannels: Float32Array[], rightChannels: Float32Array[])")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array left_input = info[0].As<Napi::Array>();
  Napi::Array right_input = info[1].As<Napi::Array>();
  const size_t count = left_input.Length();
  if (right_input.Length() != count) {
    Napi::TypeError::New(env, "leftChannels and rightChannels must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Float32Array> left_arrays;
  std::vector<Napi::Float32Array> right_arrays;
  std::vector<const float*> left_ptrs;
  std::vector<const float*> right_ptrs;
  left_arrays.reserve(count);
  right_arrays.reserve(count);
  left_ptrs.reserve(count);
  right_ptrs.reserve(count);

  size_t length = 0;
  for (size_t index = 0; index < count; ++index) {
    Napi::Value left_value = left_input.Get(index);
    Napi::Value right_value = right_input.Get(index);
    if (!IsFloat32Array(left_value) || !IsFloat32Array(right_value)) {
      Napi::TypeError::New(env, "all channels must be Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_arrays.push_back(left_value.As<Napi::Float32Array>());
    right_arrays.push_back(right_value.As<Napi::Float32Array>());
    if (left_arrays.back().ElementLength() != right_arrays.back().ElementLength()) {
      Napi::TypeError::New(env, "left and right channel lengths must match")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (index == 0) {
      length = left_arrays.back().ElementLength();
    } else if (left_arrays.back().ElementLength() != length) {
      Napi::TypeError::New(env, "all strips must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_ptrs.push_back(left_arrays.back().Data());
    right_ptrs.push_back(right_arrays.back().Data());
  }

  if (length > static_cast<size_t>(block_size_)) {
    Napi::TypeError::New(env, "block length exceeds the mixer's configured block size")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  SonareError err = sonare_mixer_process_stereo(mixer_, count > 0 ? left_ptrs.data() : nullptr,
                                                count > 0 ? right_ptrs.data() : nullptr, count,
                                                left_out.Data(), right_out.Data(), length);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("mixer process failed: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("sampleRate", sample_rate_);
  return out;
}

Napi::Value MixerWrap::StripCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(sonare_mixer_strip_count(mixer_)));
}

Napi::Value MixerWrap::ScheduleInsertAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 5 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (stripIndex, insertIndex, paramId, samplePos, value, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const size_t strip_index = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  const unsigned int insert_index = info[1].As<Napi::Number>().Uint32Value();
  const unsigned int param_id = info[2].As<Napi::Number>().Uint32Value();
  const int64_t sample_pos = info[3].As<Napi::Number>().Int64Value();
  const float value = info[4].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 0;

  SonareStrip* strip = sonare_mixer_strip_at(mixer_, strip_index);
  if (strip == nullptr) {
    Napi::Error::New(env, "mixer strip index out of range").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareError err = sonare_strip_schedule_insert_automation(strip, insert_index, param_id,
                                                            sample_pos, value, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(
        env, std::string("failed to schedule insert automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ToSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
  if (err != SONARE_OK || json == nullptr) {
    Napi::Error::New(env,
                     std::string("failed to serialize mixer scene: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
}

void MixerWrap::Destroy(const Napi::CallbackInfo& /*info*/) {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

}  // namespace sonare_node
