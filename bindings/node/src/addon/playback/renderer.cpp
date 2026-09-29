#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_options.h"
#include "sonare_wrap_playback.h"
#include "sonare_wrap_utils.h"

Napi::Object PlaybackRendererWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "PlaybackRenderer",
      {
          InstanceMethod<&PlaybackRendererWrap::ProcessPlanar>("processPlanar"),
          InstanceMethod<&PlaybackRendererWrap::ProcessInterleaved>("processInterleaved"),
          InstanceMethod<&PlaybackRendererWrap::SetConfig>("setConfig"),
          InstanceMethod<&PlaybackRendererWrap::Config>("config"),
          InstanceMethod<&PlaybackRendererWrap::SetHeadOrientation>("setHeadOrientation"),
          InstanceMethod<&PlaybackRendererWrap::Reset>("reset"),
          InstanceMethod<&PlaybackRendererWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&PlaybackRendererWrap::InputChannels>("inputChannels"),
          InstanceMethod<&PlaybackRendererWrap::OutputChannels>("outputChannels"),
          InstanceMethod<&PlaybackRendererWrap::Diagnostics>("diagnostics"),
          InstanceMethod<&PlaybackRendererWrap::NonFiniteDiscardCount>("nonFiniteDiscardCount"),
          InstanceMethod<&PlaybackRendererWrap::Destroy>("destroy"),
      });

  exports.Set("PlaybackRenderer", func);
  return exports;
}

PlaybackRendererWrap::PlaybackRendererWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<PlaybackRendererWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (configJson, hrtf?, sampleRate?, maxBlockSize?)")
        .ThrowAsJavaScriptException();
    return;
  }
  const std::string config_json = info[0].As<Napi::String>().Utf8Value();
  const SonareHrtfSet* hrtf = nullptr;
  if (!HrtfSetWrap::ReadHandle(env, info[1], &hrtf)) return;
  int sample_rate = 48000;
  int max_block_size = 1024;
  if (!sonare_node::OptionalIntArg(env, info, 2, "sampleRate", 48000, &sample_rate) ||
      !sonare_node::OptionalIntArg(env, info, 3, "maxBlockSize", 1024, &max_block_size)) {
    return;
  }
  sonare_node::ThrowIfError(
      env, sonare_playback_renderer_create_json(config_json.c_str(), hrtf, sample_rate,
                                                max_block_size, &renderer_));
  SONARE_NODE_CATCH_VOID(env)
}

PlaybackRendererWrap::~PlaybackRendererWrap() {
  sonare_playback_renderer_destroy(renderer_);
  renderer_ = nullptr;
}

bool PlaybackRendererWrap::EnsureAlive(Napi::Env env) const {
  if (renderer_ != nullptr) return true;
  Napi::Error::New(env, "PlaybackRenderer has been destroyed").ThrowAsJavaScriptException();
  return false;
}

Napi::Value PlaybackRendererWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  sonare_playback_renderer_destroy(renderer_);
  renderer_ = nullptr;
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::ProcessPlanar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Expected (Float32Array[])").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Array planes = info[0].As<Napi::Array>();
  const uint32_t in_channels = planes.Length();
  if (in_channels == 0) {
    Napi::RangeError::New(env, "planes must not be empty").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<const float*> in_pointers(in_channels);
  int frames = -1;
  for (uint32_t ch = 0; ch < in_channels; ++ch) {
    Napi::Value value = planes.Get(ch);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "each plane must be a Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Float32Array plane = value.As<Napi::Float32Array>();
    if (frames < 0) {
      frames = static_cast<int>(plane.ElementLength());
    } else if (static_cast<int>(plane.ElementLength()) != frames) {
      Napi::RangeError::New(env, "every plane must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    in_pointers[ch] = plane.Data();
  }

  int out_channels = 0;
  sonare_node::ThrowIfError(
      env, sonare_playback_renderer_output_channel_count(renderer_, &out_channels));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Array outputs = Napi::Array::New(env, static_cast<uint32_t>(out_channels));
  std::vector<float*> out_pointers(static_cast<size_t>(out_channels));
  for (int ch = 0; ch < out_channels; ++ch) {
    Napi::Float32Array plane = Napi::Float32Array::New(env, static_cast<size_t>(frames));
    out_pointers[static_cast<size_t>(ch)] = plane.Data();
    outputs.Set(static_cast<uint32_t>(ch), plane);
  }

  // Realtime entry: see the note in sonare_wrap_playback.h.
  sonare_node::ThrowIfRealtimeError(
      env, sonare_playback_renderer_process_planar(renderer_, in_pointers.data(),
                                                   static_cast<int>(in_channels),
                                                   out_pointers.data(), out_channels, frames));
  if (env.IsExceptionPending()) return env.Undefined();
  return outputs;
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::ProcessInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!sonare_node::RequireFloat32Array(info, 0, "Expected (Float32Array, inChannels)")) {
    return env.Undefined();
  }
  int in_channels = 0;
  if (!sonare_node::RequiredIntArg(env, info, 1, "inChannels", &in_channels))
    return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  if (in_channels <= 0 || input.ElementLength() % static_cast<size_t>(in_channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(in_channels);

  int out_channels = 0;
  sonare_node::ThrowIfError(
      env, sonare_playback_renderer_output_channel_count(renderer_, &out_channels));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Float32Array output =
      Napi::Float32Array::New(env, frames * static_cast<size_t>(out_channels));
  // Realtime entry: see the note in sonare_wrap_playback.h.
  sonare_node::ThrowIfRealtimeError(env, sonare_playback_renderer_process_interleaved(
                                             renderer_, input.Data(), in_channels, output.Data(),
                                             out_channels, static_cast<int>(frames)));
  if (env.IsExceptionPending()) return env.Undefined();
  return output;
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (configJson)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const std::string config_json = info[0].As<Napi::String>().Utf8Value();
  sonare_node::ThrowIfError(
      env, sonare_playback_renderer_set_config_json(renderer_, config_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::Config(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  char* json = nullptr;
  sonare_node::ThrowIfError(env, sonare_playback_renderer_config_json(renderer_, &json));
  if (env.IsExceptionPending()) return env.Undefined();
  return sonare_node::ParseJsonObjectAndFree(env, json,
                                             "Failed to parse playback renderer config JSON");
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::SetHeadOrientation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  float yaw_deg = 0.0f;
  if (!sonare_node::RequiredFloatValue(env, info[0], "yawDeg", &yaw_deg)) return env.Undefined();
  float pitch_deg = 0.0f;
  float roll_deg = 0.0f;
  if (!sonare_node::OptionalFloatArg(env, info, 1, "pitchDeg", 0.0f, &pitch_deg) ||
      !sonare_node::OptionalFloatArg(env, info, 2, "rollDeg", 0.0f, &roll_deg)) {
    return env.Undefined();
  }
  SONARE_NODE_TRY
  // Realtime entry: see the note in sonare_wrap_playback.h.
  sonare_node::ThrowIfRealtimeError(
      env, sonare_playback_renderer_set_head_orientation(renderer_, yaw_deg, pitch_deg, roll_deg));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  sonare_node::ThrowIfError(env, sonare_playback_renderer_reset(renderer_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  int out = 0;
  sonare_node::ThrowIfError(env, sonare_playback_renderer_latency_samples(renderer_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::InputChannels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  int out = 0;
  sonare_node::ThrowIfError(env, sonare_playback_renderer_input_channel_count(renderer_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::OutputChannels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  int out = 0;
  sonare_node::ThrowIfError(env, sonare_playback_renderer_output_channel_count(renderer_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::Diagnostics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  char* json = nullptr;
  sonare_node::ThrowIfError(env, sonare_playback_renderer_diagnostics_json(renderer_, &json));
  if (env.IsExceptionPending()) return env.Undefined();
  return sonare_node::ParseJsonObjectAndFree(env, json,
                                             "Failed to parse playback diagnostics JSON");
  SONARE_NODE_CATCH(env)
}

Napi::Value PlaybackRendererWrap::NonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(env)) return env.Undefined();
  uint32_t count = 0;
  sonare_node::ThrowIfError(env,
                            sonare_playback_renderer_non_finite_discard_count(renderer_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, count);
  SONARE_NODE_CATCH(env)
}
