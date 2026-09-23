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
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Chirp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Clicks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db = node_arg_finite_float(info, 1, 60.0f);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db = node_arg_finite_float(info, 1, 60.0f);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_split_silence(arr.Data(), arr.ElementLength(), top_db, frame_length,
                                         hop_length, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

namespace {

// The two split_silence_common entry points take the same arguments and refuse
// the same inputs; sharing one body is what makes that true rather than claimed.
// `with_report` picks the `_ex` call and wraps the intervals in an object.
Napi::Value SplitSilenceCommonImpl(const Napi::CallbackInfo& info, bool with_report) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Expected signals: Float32Array[]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array signals_input = info[0].As<Napi::Array>();
  const size_t count = signals_input.Length();
  if (count == 0) {
    Napi::TypeError::New(env, "signals must not be empty").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Reserved up front: the loop below takes each element's Data() pointer, and
  // a reallocation mid-loop would invalidate the ones already taken.
  std::vector<Napi::Float32Array> signal_arrays;
  std::vector<const float*> signal_ptrs;
  std::vector<size_t> lengths;
  signal_arrays.reserve(count);
  signal_ptrs.reserve(count);
  lengths.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Value value = signals_input.Get(index);
    if (!IsFloat32Array(value)) {
      Napi::TypeError::New(env, "signals must contain only Float32Array elements")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    signal_arrays.push_back(value.As<Napi::Float32Array>());
    signal_ptrs.push_back(signal_arrays.back().Data());
    lengths.push_back(signal_arrays.back().ElementLength());
  }

  float top_db = node_arg_finite_float(info, 1, 60.0f);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  int* out = nullptr;
  size_t out_count = 0;
  SonareSilenceCommonReport report{};
  const SonareError err =
      with_report
          ? sonare_split_silence_common_ex(signal_ptrs.data(), signal_ptrs.size(), lengths.data(),
                                           top_db, frame_length, hop_length, &out, &out_count,
                                           &report)
          : sonare_split_silence_common(signal_ptrs.data(), signal_ptrs.size(), lengths.data(),
                                        top_db, frame_length, hop_length, &out, &out_count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Value intervals = IntResult(env, out, out_count);
  if (!with_report) return intervals;
  Napi::Object report_object = Napi::Object::New(env);
  report_object.Set("silenceCeilingDb", report.silence_ceiling_db);
  report_object.Set("maxSignalIntervals", report.max_signal_intervals);
  report_object.Set("minSignalIntervals", report.min_signal_intervals);
  Napi::Object result = Napi::Object::New(env);
  result.Set("intervals", intervals);
  result.Set("report", report_object);
  return result;
}

}  // namespace

Napi::Value SonareWrap::SplitSilenceCommon(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return SplitSilenceCommonImpl(info, false);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitSilenceCommonWithReport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return SplitSilenceCommonImpl(info, true);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FrameSignal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, frameLength, hopLength)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_frame_signal(
      arr.Data(), arr.ElementLength(), node_narrow_int(env, info[1], node_arg_label(1).c_str()),
      node_narrow_int(env, info[2], node_arg_label(2).c_str()), &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("frames", FloatResult(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PadCenter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value = node_arg_finite_float(info, 2, 0.0f);
  const int64_t target_size = node_narrow_int64(env, info[1], "targetSize");
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FixLength(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value = node_arg_finite_float(info, 2, 0.0f);
  const int64_t target_size = node_narrow_int64(env, info[1], "targetSize");
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FixFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected frames").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<int> frames = IntVectorFromValue(info[0], "frames");
  int x_min = node_arg_int(info, 1, 0);
  int x_max = node_arg_int(info, 2, -1);
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
  const std::vector<int> events = IntVectorFromValue(info[0], "events");
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
  SONARE_NODE_TRY
  if (info.Length() < 7 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (values, preMax, postMax, preAvg, postAvg, delta, wait)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  // Six numeric arguments read back to back: the inline form left the first bad
  // one pending and let the next one's throw land on top of it, which aborts.
  int pre_max = 0;
  int post_max = 0;
  int pre_avg = 0;
  int post_avg = 0;
  float delta = 0.0f;
  int wait = 0;
  if (!RequiredIntArg(env, info, 1, "preMax", &pre_max) ||
      !RequiredIntArg(env, info, 2, "postMax", &post_max) ||
      !RequiredIntArg(env, info, 3, "preAvg", &pre_avg) ||
      !RequiredIntArg(env, info, 4, "postAvg", &post_avg) ||
      !RequiredFloatValue(env, info[5], "delta", &delta) ||
      !RequiredIntArg(env, info, 6, "wait", &wait)) {
    return env.Undefined();
  }
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_peak_pick(arr.Data(), arr.ElementLength(), pre_max, post_max, pre_avg,
                                     post_avg, delta, wait, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VectorNormalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int norm_type = node_arg_int(info, 1, 0);
  float threshold = node_arg_finite_float(info, 2, 0.0f);
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_vector_normalize(arr.Data(), arr.ElementLength(), norm_type, threshold, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Pcen(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    sr = IntProperty(opts, "sampleRate", sr);
    hop = IntProperty(opts, "hopLength", hop);
    time_constant = FloatProperty(opts, "timeConstant", time_constant);
    gain = FloatProperty(opts, "gain", gain);
    bias = FloatProperty(opts, "bias", bias);
    power = FloatProperty(opts, "power", power);
    eps = FloatProperty(opts, "eps", eps);
  }
  const int n_bins = node_narrow_int(env, info[1], "nBins");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "pcen", n_bins, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pcen(arr.Data(), n_bins, n_frames, sr, hop, time_constant, gain, bias,
                                power, eps, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}
