#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "features/common.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Tone(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const float frequency = node_arg_float(info, 0, 440.0f);
  const int sample_rate = node_arg_int(info, 1, 22050);
  const float duration = node_arg_float(info, 2, 1.0f);
  const float phase = node_arg_float(info, 3, 0.0f);
  const float amplitude = node_arg_float(info, 4, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error =
      sonare_tone(frequency, sample_rate, duration, phase, amplitude, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return FloatResult(env, out, out_length);
}

Napi::Value SonareWrap::Chirp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const float fmin = node_arg_float(info, 0, 440.0f);
  const float fmax = node_arg_float(info, 1, 880.0f);
  const int sample_rate = node_arg_int(info, 2, 22050);
  const float duration = node_arg_float(info, 3, 1.0f);
  const bool linear = node_arg_bool(info, 4, true);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error =
      sonare_chirp(fmin, fmax, sample_rate, duration, linear ? 1 : 0, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return FloatResult(env, out, out_length);
}

Napi::Value SonareWrap::Clicks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected click times Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const auto times = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_arg_int(info, 1, 22050);
  const int length = node_arg_int(info, 2, 0);
  const float frequency = node_arg_float(info, 3, 1000.0f);
  const float duration = node_arg_float(info, 4, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error = sonare_clicks(times.Data(), times.ElementLength(), sample_rate, length,
                                          frequency, duration, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return FloatResult(env, out, out_length);
}

Napi::Value SonareWrap::TrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 60.0f;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float* out = nullptr;
  size_t count = 0;
  int start = 0;
  int end = 0;
  SonareError err = sonare_trim_silence(arr.Data(), arr.ElementLength(), top_db, frame_length,
                                        hop_length, &out, &count, &start, &end);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("audio", FloatResult(env, out, count));
  result.Set("startSample", start);
  result.Set("endSample", end);
  return result;
}

Napi::Value SonareWrap::SplitSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 60.0f;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_split_silence(arr.Data(), arr.ElementLength(), top_db, frame_length,
                                         hop_length, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
}

Napi::Value SonareWrap::FrameSignal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, frameLength, hopLength)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err =
      sonare_frame_signal(arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int32Value(),
                          info[2].As<Napi::Number>().Int32Value(), &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("frames", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::PadCenter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  const int64_t target_size = info[1].As<Napi::Number>().Int64Value();
  if (target_size < 0) {
    Napi::RangeError::New(env, "size must be non-negative").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pad_center(arr.Data(), arr.ElementLength(),
                                      static_cast<size_t>(target_size), pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::FixLength(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  const int64_t target_size = info[1].As<Napi::Number>().Int64Value();
  if (target_size < 0) {
    Napi::RangeError::New(env, "size must be non-negative").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_fix_length(arr.Data(), arr.ElementLength(),
                                      static_cast<size_t>(target_size), pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::FixFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected frames").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<int> frames = IntVectorFromValue(info[0]);
  int x_min =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 0;
  int x_max =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : -1;
  bool pad = info.Length() >= 4 && info[3].IsBoolean() ? info[3].As<Napi::Boolean>().Value() : true;
  int* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_fix_frames(frames.data(), frames.size(), x_min, x_max, pad ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::OnsetBacktrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (events, energy)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const std::vector<int> events = IntVectorFromValue(info[0]);
  const auto energy = info[1].As<Napi::Float32Array>();
  int* out = nullptr;
  size_t count = 0;
  const SonareError err = sonare_onset_backtrack(events.data(), events.size(), energy.Data(),
                                                 energy.ElementLength(), &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PeakPick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 7 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (values, preMax, postMax, preAvg, postAvg, delta, wait)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_peak_pick(
      arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int32Value(),
      info[2].As<Napi::Number>().Int32Value(), info[3].As<Napi::Number>().Int32Value(),
      info[4].As<Napi::Number>().Int32Value(), info[5].As<Napi::Number>().FloatValue(),
      info[6].As<Napi::Number>().Int32Value(), &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
}

Napi::Value SonareWrap::VectorNormalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int norm_type =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 0;
  float threshold =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_vector_normalize(arr.Data(), arr.ElementLength(), norm_type, threshold, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Pcen(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, nBins, nFrames, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = 22050, hop = 512;
  float time_constant = 0.4f, gain = 0.98f, bias = 2.0f, power = 0.5f, eps = 1e-6f;
  if (info.Length() >= 4 && info[3].IsObject()) {
    auto opts = info[3].As<Napi::Object>();
    if (opts.Has("sampleRate")) sr = opts.Get("sampleRate").As<Napi::Number>().Int32Value();
    if (opts.Has("hopLength")) hop = opts.Get("hopLength").As<Napi::Number>().Int32Value();
    if (opts.Has("timeConstant"))
      time_constant = opts.Get("timeConstant").As<Napi::Number>().FloatValue();
    if (opts.Has("gain")) gain = opts.Get("gain").As<Napi::Number>().FloatValue();
    if (opts.Has("bias")) bias = opts.Get("bias").As<Napi::Number>().FloatValue();
    if (opts.Has("power")) power = opts.Get("power").As<Napi::Number>().FloatValue();
    if (opts.Has("eps")) eps = opts.Get("eps").As<Napi::Number>().FloatValue();
  }
  const int n_bins = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "pcen", n_bins, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pcen(arr.Data(), n_bins, n_frames, sr, hop, time_constant, gain, bias,
                                power, eps, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}
