#include <cstdint>
#include <cstring>
#include <string>

#include "sonare_wrap_options.h"
#include "sonare_wrap_playback.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

// `channels` names the output count, which the caller cannot know in advance
// (the source and the output target may carry a different channel count), so
// the result is `{ samples, channels }` rather than a bare Float32Array.
Napi::Value RenderPlayback(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0,
                           "Expected (Float32Array, inChannels, sampleRate, configJson, hrtf?)")) {
    return env.Undefined();
  }
  int in_channels = 0;
  int sample_rate = 0;
  if (!RequiredIntArg(env, info, 1, "inChannels", &in_channels) ||
      !RequiredIntArg(env, info, 2, "sampleRate", &sample_rate)) {
    return env.Undefined();
  }
  if (info.Length() < 4 || !info[3].IsString()) {
    Napi::TypeError::New(env, "configJson must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const SonareHrtfSet* hrtf = nullptr;
  if (!HrtfSetWrap::ReadHandle(env, info[4], &hrtf)) return env.Undefined();

  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  if (in_channels <= 0 || input.ElementLength() % static_cast<size_t>(in_channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(in_channels);
  const std::string config_json = info[3].As<Napi::String>().Utf8Value();

  float* out = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  ThrowIfError(env, sonare_playback_render_interleaved(input.Data(), frames, in_channels,
                                                       sample_rate, config_json.c_str(), hrtf, &out,
                                                       &out_frames, &out_channels));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Float32Array samples =
      Napi::Float32Array::New(env, out_frames * static_cast<size_t>(out_channels));
  std::memcpy(samples.Data(), out, samples.ElementLength() * sizeof(float));
  sonare_free_playback_render(out);

  Napi::Object result = Napi::Object::New(env);
  result.Set("samples", samples);
  result.Set("channels", Napi::Number::New(env, out_channels));
  return result;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
