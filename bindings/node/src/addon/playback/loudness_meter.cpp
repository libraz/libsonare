#include <cstdint>

#include "sonare_wrap_options.h"
#include "sonare_wrap_playback.h"
#include "sonare_wrap_utils.h"

Napi::Object PlaybackLoudnessMeterWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "PlaybackLoudnessMeter",
      {
          InstanceMethod<&PlaybackLoudnessMeterWrap::PushInterleaved>("pushInterleaved"),
          InstanceMethod<&PlaybackLoudnessMeterWrap::IntegratedLufs>("integratedLufs"),
          InstanceMethod<&PlaybackLoudnessMeterWrap::Destroy>("destroy"),
      });

  exports.Set("PlaybackLoudnessMeter", func);
  return exports;
}

PlaybackLoudnessMeterWrap::PlaybackLoudnessMeterWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<PlaybackLoudnessMeterWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int channels = 0;
  int sample_rate = 0;
  if (!sonare_node::RequiredIntArg(env, info, 0, "channels", &channels) ||
      !sonare_node::RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) {
    return;
  }
  sonare_node::ThrowIfError(env,
                            sonare_playback_loudness_meter_create(channels, sample_rate, &meter_));
  SONARE_NODE_CATCH_VOID(env)
}

PlaybackLoudnessMeterWrap::~PlaybackLoudnessMeterWrap() {
  sonare_playback_loudness_meter_destroy(meter_);
  meter_ = nullptr;
}

bool PlaybackLoudnessMeterWrap::EnsureAlive(Napi::Env env) const {
  if (meter_ != nullptr) return true;
  Napi::Error::New(env, "PlaybackLoudnessMeter has been destroyed").ThrowAsJavaScriptException();
  return false;
}

Napi::Value PlaybackLoudnessMeterWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  sonare_playback_loudness_meter_destroy(meter_);
  meter_ = nullptr;
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackLoudnessMeterWrap::PushInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!sonare_node::RequireFloat32Array(info, 0, "Expected (Float32Array)")) {
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array samples = info[0].As<Napi::Float32Array>();
  sonare_node::ThrowIfError(env, sonare_playback_loudness_meter_push_interleaved(
                                     meter_, samples.Data(), samples.ElementLength()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackLoudnessMeterWrap::IntegratedLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  float out = 0.0f;
  sonare_node::ThrowIfError(env, sonare_playback_loudness_meter_integrated_lufs(meter_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}
