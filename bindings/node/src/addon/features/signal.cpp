#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "core/synthesis.h"
#include "effects/silence.h"
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
#include "util/frame.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Tone(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  float frequency{};
  if (!RequiredFloatArg(env, info, 0, "frequency", &frequency)) return env.Undefined();
  int sample_rate{};
  if (!OptionalDomainArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kSynthesisSampleRateDomain, &sample_rate)) {
    return env.Undefined();
  }
  float duration{};
  if (!OptionalFloatArg(env, info, 2, "duration", 1.0f, &duration)) return env.Undefined();
  float phase{};
  if (!OptionalFloatArg(env, info, 3, "phase", 0.0f, &phase)) return env.Undefined();
  float amplitude{};
  if (!OptionalFloatArg(env, info, 4, "amplitude", 1.0f, &amplitude)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error =
      sonare_tone(frequency, sample_rate, duration, phase, amplitude, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Chirp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  float fmin{};
  if (!RequiredFloatArg(env, info, 0, "fmin", &fmin)) return env.Undefined();
  float fmax{};
  if (!RequiredFloatArg(env, info, 1, "fmax", &fmax)) return env.Undefined();
  int sample_rate{};
  if (!OptionalDomainArg(env, info, 2, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kSynthesisSampleRateDomain, &sample_rate)) {
    return env.Undefined();
  }
  float duration{};
  if (!OptionalFloatArg(env, info, 3, "duration", 1.0f, &duration)) return env.Undefined();
  bool linear{};
  if (!RequiredBoolArg(env, info, 4, "linear", &linear)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error =
      sonare_chirp(fmin, fmax, sample_rate, duration, linear ? 1 : 0, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Clicks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "times must be a Float32Array")) return env.Undefined();
  const auto times = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!OptionalDomainArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kSynthesisSampleRateDomain, &sample_rate)) {
    return env.Undefined();
  }
  int length{};
  if (!OptionalDomainArg(env, info, 2, "length", 0, sonare::kClicksLengthDomain, &length)) {
    return env.Undefined();
  }
  float frequency{};
  if (!OptionalFloatArg(env, info, 3, "frequency", 1000.0f, &frequency)) return env.Undefined();
  float duration{};
  if (!OptionalFloatArg(env, info, 4, "duration", 0.1f, &duration)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError error =
      sonare_clicks(sonare_node::ElementData(times), times.ElementLength(), sample_rate, length,
                    frequency, duration, &out, &out_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db{};
  if (!RequiredFloatValue(env, info[1], "topDb", &top_db)) return env.Undefined();
  int frame_length{};
  if (!RequiredDomainArg(env, info, 2, "frameLength", sonare::kSilenceFrameLengthDomain,
                         &frame_length)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kSilenceHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int64_t start = 0;
  int64_t end = 0;
  SonareError err = sonare_trim_silence(sonare_node::ElementData(arr), arr.ElementLength(), top_db,
                                        frame_length, hop_length, &out, &count, &start, &end);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("audio", TakeCFloats(env, out, count));
  result.Set("startSample", Napi::Number::New(env, static_cast<double>(start)));
  result.Set("endSample", Napi::Number::New(env, static_cast<double>(end)));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db{};
  if (!RequiredFloatValue(env, info[1], "topDb", &top_db)) return env.Undefined();
  int frame_length{};
  if (!RequiredDomainArg(env, info, 2, "frameLength", sonare::kSilenceFrameLengthDomain,
                         &frame_length)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kSilenceHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_split_silence(sonare_node::ElementData(arr), arr.ElementLength(), top_db,
                                         frame_length, hop_length, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCInts(env, out, count);
  SONARE_NODE_CATCH(env)
}

// A trailing `withReport` of true wraps the intervals with the report of why they are the gaps.
Napi::Value SonareWrap::SplitSilenceCommon(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "signals must be an array of Float32Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array signals_input = info[0].As<Napi::Array>();
  const size_t count = node_list_length(signals_input, "signals");
  if (count == 0) {
    Napi::RangeError::New(env, "signals must not be empty").ThrowAsJavaScriptException();
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
    signal_ptrs.push_back(sonare_node::ElementData(signal_arrays.back()));
    lengths.push_back(signal_arrays.back().ElementLength());
  }

  float top_db{};
  if (!OptionalFloatArg(env, info, 1, "topDb", 60.0f, &top_db)) return env.Undefined();
  int frame_length{};
  if (!OptionalDomainArg(env, info, 2, "frameLength", 2048, sonare::kSilenceFrameLengthDomain,
                         &frame_length)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kSilenceHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  bool with_report = false;
  if (!OptionalBoolArg(env, info, 4, "withReport", false, &with_report)) return env.Undefined();
  int* out = nullptr;
  size_t out_count = 0;
  SonareSilenceCommonReport report{};
  const SonareError err = sonare_split_silence_common(
      sonare_node::ElementData(signal_ptrs), signal_ptrs.size(), sonare_node::ElementData(lengths),
      top_db, frame_length, hop_length, &out, &out_count, with_report ? &report : nullptr);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Value intervals = TakeCInts(env, out, out_count);
  if (!with_report) return intervals;
  Napi::Object report_object = Napi::Object::New(env);
  SetMeasured(report_object, "silenceCeilingDb", report.silence_ceiling_db);
  report_object.Set("maxSignalIntervals", report.max_signal_intervals);
  report_object.Set("minSignalIntervals", report.min_signal_intervals);
  Napi::Object result = Napi::Object::New(env);
  result.Set("intervals", intervals);
  result.Set("report", report_object);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FrameSignal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int frame_length{};
  if (!RequiredDomainArg(env, info, 1, "frameLength", sonare::kFrameLengthDomain, &frame_length)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!RequiredDomainArg(env, info, 2, "hopLength", sonare::kFrameHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_frame_signal(sonare_node::ElementData(arr), arr.ElementLength(),
                                        frame_length, hop_length, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("frames", TakeCFloats(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PadCenter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) return env.Undefined();
  size_t target_size = 0;
  if (!NonNegativeSizeTArg(env, info, 1, "targetSize", &target_size)) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value{};
  if (!OptionalFloatArg(env, info, 2, "padValue", 0.0f, &pad_value)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pad_center(sonare_node::ElementData(arr), arr.ElementLength(),
                                      target_size, pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FixLength(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) return env.Undefined();
  size_t target_size = 0;
  if (!NonNegativeSizeTArg(env, info, 1, "targetSize", &target_size)) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value{};
  if (!OptionalFloatArg(env, info, 2, "padValue", 0.0f, &pad_value)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_fix_length(sonare_node::ElementData(arr), arr.ElementLength(),
                                      target_size, pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FixFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "frames must be an Int32Array or number[]")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<int> frames = IntVectorFromValue(info[0], "frames");
  int x_min{};
  if (!OptionalIntArg(env, info, 1, "xMin", 0, &x_min)) return env.Undefined();
  int x_max{};
  if (!OptionalIntArg(env, info, 2, "xMax", -1, &x_max)) return env.Undefined();
  bool pad = true;
  if (!OptionalBoolArg(env, info, 3, "pad", true, &pad)) {
    return env.Undefined();
  }
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_fix_frames(sonare_node::ElementData(frames), frames.size(), x_min, x_max,
                                      pad ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCInts(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::OnsetBacktrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const std::vector<int> events = IntVectorFromValue(info[0], "events");
  if (!RequireFloat32Array(info, 1, "energy must be a Float32Array")) return env.Undefined();
  const auto energy = info[1].As<Napi::Float32Array>();
  int* out = nullptr;
  size_t count = 0;
  const SonareError err = sonare_onset_backtrack(sonare_node::ElementData(events), events.size(),
                                                 sonare_node::ElementData(energy),
                                                 energy.ElementLength(), &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCInts(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PeakPick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) return env.Undefined();
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
  SonareError err = sonare_peak_pick(sonare_node::ElementData(arr), arr.ElementLength(), pre_max,
                                     post_max, pre_avg, post_avg, delta, wait, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCInts(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VectorNormalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int norm_type{};
  if (!RequiredIntArg(env, info, 1, "normType", &norm_type)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 2, "threshold", 0.0f, &threshold)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_vector_normalize(sonare_node::ElementData(arr), arr.ElementLength(),
                              static_cast<SonareNormType>(norm_type), threshold, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Pcen(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) return env.Undefined();
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "nBins must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "nFrames must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = sonare::constants::kDefaultSampleRate, hop = 512;
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
  if (!ValidateMatrixDims(env, n_bins, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pcen(sonare_node::ElementData(arr), n_bins, n_frames, sr, hop,
                                time_constant, gain, bias, power, eps, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Preemphasis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef{};
  if (!RequiredFloatValue(env, info[1], "coef", &coef)) return env.Undefined();
  bool use_zi = info.Length() >= 3 && info[2].IsNumber();
  float zi{};
  if (!OptionalFloatArg(env, info, 2, "zi", 0.0f, &zi)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_preemphasis(sonare_node::ElementData(arr), arr.ElementLength(), coef, zi,
                                       use_zi ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Deemphasis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef{};
  if (!RequiredFloatValue(env, info[1], "coef", &coef)) return env.Undefined();
  bool use_zi = info.Length() >= 3 && info[2].IsNumber();
  float zi{};
  if (!OptionalFloatArg(env, info, 2, "zi", 0.0f, &zi)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_deemphasis(sonare_node::ElementData(arr), arr.ElementLength(), coef, zi,
                                      use_zi ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}
