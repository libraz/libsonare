#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "c_api/editing_input_domains.h"
#include "c_api/sonare_c_internal.h"
#include "core/audio.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "metering/clipping.h"
#include "metering/waveform.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

namespace {

template <typename T, void (*FreeFn)(T*)>
class CResultGuard {
 public:
  explicit CResultGuard(T* result) : result_(result) {}
  CResultGuard(const CResultGuard&) = delete;
  CResultGuard& operator=(const CResultGuard&) = delete;
  ~CResultGuard() {
    if (result_) FreeFn(result_);
  }

 private:
  T* result_;
};

// Marshals a SonareSpectrumResult into the JS spectrum object shape shared by
// meteringSpectrum (Welch-averaged) and meteringSpectrumFrame (single frame).
Napi::Value EmitSpectrumResult(Napi::Env env, const SonareSpectrumResult& result) {
  auto freq = Float32ArrayFrom(env, result.frequencies, result.bin_count);
  auto mag = Float32ArrayFrom(env, result.magnitude, result.bin_count);
  auto pwr = Float32ArrayFrom(env, result.power, result.bin_count);
  auto db = Float32ArrayFrom(env, result.db, result.bin_count);
  Napi::Object out = Napi::Object::New(env);
  out.Set("frequencies", freq);
  out.Set("magnitude", mag);
  out.Set("power", pwr);
  out.Set("db", db);
  out.Set("nFft", Napi::Number::New(env, result.n_fft));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  return out;
}

Napi::Value EmitWaveformPeaksResult(Napi::Env env, const SonareWaveformPeaksResult& result) {
  const size_t total = static_cast<size_t>(result.num_channels) * result.bucket_count;
  auto min_values = Float32ArrayFrom(env, result.min, total);
  auto max_values = Float32ArrayFrom(env, result.max, total);
  Napi::Object out = Napi::Object::New(env);
  out.Set("min", min_values);
  out.Set("max", max_values);
  out.Set("channels", Napi::Number::New(env, result.num_channels));
  out.Set("bucketCount", Napi::Number::New(env, result.bucket_count));
  out.Set("samplesPerBucket", Napi::Number::New(env, result.samples_per_bucket));
  return out;
}

}  // namespace

Napi::Value SonareWrap::Lufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();

  SonareLufsResult lufs{};
  SonareError err = sonare_lufs(sonare_node::ElementData(typed), typed.ElementLength(), sr, &lufs);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  SetMeasured(result, "momentaryLufs", lufs.momentary_lufs);
  SetMeasured(result, "shortTermLufs", lufs.short_term_lufs);
  SetMeasured(result, "maxMomentaryLufs", lufs.max_momentary_lufs);
  SetMeasured(result, "maxShortTermLufs", lufs.max_short_term_lufs);
  SetMeasured(result, "loudnessRangeLu", lufs.loudness_range_lu);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MomentaryLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_momentary_lufs(sonare_node::ElementData(typed), typed.ElementLength(),
                                          sr, &out, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ShortTermLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_short_term_lufs(sonare_node::ElementData(typed), typed.ElementLength(),
                                           sr, &out, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::LufsInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int channels{};
  if (!RequiredDomainArg(env, info, 1, "channels", sonare_c_detail::kInterleavedChannelsDomain,
                         &channels)) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 2, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  if (typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "interleaved length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t frames = typed.ElementLength() / static_cast<size_t>(channels);

  SonareLufsResult lufs{};
  SonareError err =
      sonare_lufs_interleaved(sonare_node::ElementData(typed), frames, channels, sr, &lufs);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  SetMeasured(result, "momentaryLufs", lufs.momentary_lufs);
  SetMeasured(result, "shortTermLufs", lufs.short_term_lufs);
  SetMeasured(result, "maxMomentaryLufs", lufs.max_momentary_lufs);
  SetMeasured(result, "maxShortTermLufs", lufs.max_short_term_lufs);
  SetMeasured(result, "loudnessRangeLu", lufs.loudness_range_lu);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::LufsSeriesInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int channels{};
  if (!RequiredDomainArg(env, info, 1, "channels", sonare_c_detail::kInterleavedChannelsDomain,
                         &channels)) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 2, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  if (typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "interleaved length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t frames = typed.ElementLength() / static_cast<size_t>(channels);

  float* momentary = nullptr;
  size_t momentary_count = 0;
  float* short_term = nullptr;
  size_t short_term_count = 0;
  SonareError err =
      sonare_lufs_series_interleaved(sonare_node::ElementData(typed), frames, channels, sr,
                                     &momentary, &momentary_count, &short_term, &short_term_count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("momentary", TakeCFloats(env, momentary, momentary_count));
  result.Set("shortTerm", TakeCFloats(env, short_term, short_term_count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Ebur128LoudnessRangeLu(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();

  float out_lra = 0.0f;
  SonareError err = sonare_ebur128_loudness_range_lu(sonare_node::ElementData(typed),
                                                     typed.ElementLength(), sr, &out_lra);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return MeasuredNumber(env, out_lra);
  SONARE_NODE_CATCH(env)
}

namespace {

// Shared shape for the scalar offline meters (peak/rms/crest/dc).
using MeteringScalarFn = SonareError (*)(const float*, size_t, int, float*);

Napi::Value MeteringScalar(const Napi::CallbackInfo& info, MeteringScalarFn fn) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = fn(sonare_node::ElementData(typed), typed.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return MeasuredNumber(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringPeakDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return MeteringScalar(info, &sonare_metering_peak_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringRmsDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return MeteringScalar(info, &sonare_metering_rms_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringSilenceRatio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto samples = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return env.Undefined();
  float threshold_db{};
  if (!OptionalFloatArg(env, info, 2, "thresholdDb", -45.0f, &threshold_db)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 3, "frameLength", 1024, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 4, "hopLength", 256, &hop_length)) return env.Undefined();
  float ratio = 0.0f;
  const SonareError err =
      sonare_metering_silence_ratio(sonare_node::ElementData(samples), samples.ElementLength(),
                                    sample_rate, threshold_db, frame_length, hop_length, &ratio);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, ratio);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringCrestFactorDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return MeteringScalar(info, &sonare_metering_crest_factor_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringDcOffset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return MeteringScalar(info, &sonare_metering_dc_offset);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringTruePeakDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int oversample{};
  if (!OptionalIntArg(env, info, 2, "oversampleFactor", 4, &oversample)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = sonare_metering_true_peak_dbtp(
      sonare_node::ElementData(typed), typed.ElementLength(), sr, oversample, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringDetectClipping(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  float threshold{};
  // Absent is the C ABI's NaN, the library's clip level; a written NaN is refused.
  if (!OptionalDomainArg(env, info, 2, "thresholdDb", std::numeric_limits<float>::quiet_NaN(),
                         sonare::metering::kClippingThresholdDbDomain, &threshold))
    return env.Undefined();
  int64_t min_region_value{};
  if (!OptionalInt64Arg(env, info, 3, "minRegionSamples", 1, &min_region_value))
    return env.Undefined();
  if (min_region_value < 0) {
    Napi::RangeError::New(env, "minRegionSamples must be an integer of at least 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t min_region = static_cast<size_t>(min_region_value);
  SonareClippingResult result{};
  SonareError err = sonare_metering_detect_clipping(
      sonare_node::ElementData(typed), typed.ElementLength(), sr, threshold, min_region, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareClippingResult, sonare_free_clipping_result> guard(&result);
  Napi::Array regions = Napi::Array::New(env, result.region_count);
  for (size_t i = 0; i < result.region_count; ++i) {
    Napi::Object region = Napi::Object::New(env);
    region.Set("startSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].start_sample)));
    region.Set("endSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].end_sample)));
    region.Set("length", Napi::Number::New(env, static_cast<double>(result.regions[i].length)));
    region.Set("peak", Napi::Number::New(env, result.regions[i].peak));
    regions.Set(static_cast<uint32_t>(i), region);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("clippedSamples", Napi::Number::New(env, static_cast<double>(result.clipped_samples)));
  out.Set("clippingRatio", Napi::Number::New(env, result.clipping_ratio));
  out.Set("maxClippedPeak", Napi::Number::New(env, result.max_clipped_peak));
  out.Set("regions", regions);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringDynamicRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  float window_sec{};
  if (!RequiredFloatValue(env, info[2], "windowSec", &window_sec)) return env.Undefined();
  float hop_sec{};
  if (!RequiredFloatValue(env, info[3], "hopSec", &hop_sec)) return env.Undefined();
  // The C ABI treats 0.0 as a literal 0th percentile and a NEGATIVE value as
  // "use the library default", so omitted percentiles pass -1.0f (default).
  float low_p{};
  if (!RequiredFloatValue(env, info[4], "lowP", &low_p)) return env.Undefined();
  float high_p{};
  if (!RequiredFloatValue(env, info[5], "highP", &high_p)) return env.Undefined();
  SonareDynamicRangeResult result{};
  SonareError err =
      sonare_metering_dynamic_range(sonare_node::ElementData(typed), typed.ElementLength(), sr,
                                    window_sec, hop_sec, low_p, high_p, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareDynamicRangeResult, sonare_free_dynamic_range_result> guard(&result);
  auto windows = Float32ArrayFrom(env, result.window_rms_db, result.window_count);
  Napi::Object out = Napi::Object::New(env);
  SetMeasured(out, "dynamicRangeDb", result.dynamic_range_db);
  SetMeasured(out, "lowPercentileDb", result.low_percentile_db);
  SetMeasured(out, "highPercentileDb", result.high_percentile_db);
  out.Set("windowRmsDb", windows);
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

bool ParseScaleArgs(const Napi::CallbackInfo& info, int* root, uint16_t* mode_mask,
                    float* reference_midi, float* midi) {
  Napi::Env env = info.Env();
  if (!RequiredDomainArg(env, info, 0, "root", sonare::editing::pitch_editor::kScaleRootDomain,
                         root) ||
      !RequiredDomainArg(env, info, 1, "modeMask",
                         sonare::editing::pitch_editor::kScaleModeMaskDomain, mode_mask) ||
      !RequiredFloatArg(env, info, 2, "midi", midi)) {
    return false;
  }
  // Absent reads as NaN, which selects A4 (69); a given 0 is MIDI note 0.
  if (!OptionalFloatArg(info.Env(), info, 3, "scaleReferenceMidi",
                        std::numeric_limits<float>::quiet_NaN(), reference_midi)) {
    return false;
  }
  return true;
}

}  // namespace

Napi::Value SonareWrap::ScaleQuantizeMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = sonare_scale_quantize_midi(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ScaleMaskForMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int root = 0;
  if (!RequiredDomainArg(env, info, 0, "root", sonare_c_detail::kScaleMaskRootDomain, &root)) {
    return env.Undefined();
  }
  uint16_t mask = 0;
  const SonareError err =
      sonare_scale_mask_for_mode(root, node_mode_from_value(info[1], "mode"), &mask);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, mask);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ScaleCorrectionSemitones(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = sonare_scale_correction_semitones(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ScalePitchClassEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int root = 0;
  uint16_t mask = 0;
  int pitch_class = 0;
  if (!RequiredDomainArg(env, info, 0, "root", sonare::editing::pitch_editor::kScaleRootDomain,
                         &root) ||
      !RequiredDomainArg(env, info, 1, "modeMask",
                         sonare::editing::pitch_editor::kScaleModeMaskDomain, &mask) ||
      !RequiredDomainArg(env, info, 2, "pitchClass", sonare_c_detail::kScalePitchClassDomain,
                         &pitch_class)) {
    return env.Undefined();
  }
  int out_enabled = 0;
  SonareError err = sonare_scale_pitch_class_enabled(root, mask, pitch_class, &out_enabled);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Boolean::New(env, out_enabled != 0);
  SONARE_NODE_CATCH(env)
}

namespace {

using StereoScalarFn = SonareError (*)(const float*, const float*, size_t, int, float*);

Napi::Value StereoScalar(const Napi::CallbackInfo& info, StereoScalarFn fn) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "left and right must be Float32Arrays").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr{};
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = fn(sonare_node::ElementData(left), sonare_node::ElementData(right),
                       left.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return MeasuredNumber(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringStereoCorrelation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return StereoScalar(info, &sonare_metering_stereo_correlation);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringStereoWidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return StereoScalar(info, &sonare_metering_stereo_width);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringVectorscope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 2, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  size_t max_points = 0;
  if (!OptionalSizeTArg(env, info, 3, "maxPoints", 0, &max_points)) return env.Undefined();
  SonareVectorscopeResult result{};
  SonareError err =
      sonare_metering_vectorscope(sonare_node::ElementData(left), sonare_node::ElementData(right),
                                  left.ElementLength(), sr, max_points, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareVectorscopeResult, sonare_free_vectorscope_result> guard(&result);
  auto mid = Napi::Float32Array::New(env, result.point_count);
  auto side = Napi::Float32Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    mid[i] = result.points[i].mid;
    side[i] = result.points[i].side;
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("mid", mid);
  out.Set("side", side);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringPhaseScope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 2, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  size_t max_points = 0;
  if (!OptionalSizeTArg(env, info, 3, "maxPoints", 0, &max_points)) return env.Undefined();
  SonarePhaseScopeResult result{};
  SonareError err =
      sonare_metering_phase_scope(sonare_node::ElementData(left), sonare_node::ElementData(right),
                                  left.ElementLength(), sr, max_points, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonarePhaseScopeResult, sonare_free_phase_scope_result> guard(&result);
  auto mid = Napi::Float32Array::New(env, result.point_count);
  auto side = Napi::Float32Array::New(env, result.point_count);
  auto radius = Napi::Float32Array::New(env, result.point_count);
  auto angle = Napi::Float32Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    mid[i] = result.points[i].mid;
    side[i] = result.points[i].side;
    radius[i] = result.points[i].radius;
    angle[i] = result.points[i].angle_rad;
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("mid", mid);
  out.Set("side", side);
  out.Set("radius", radius);
  out.Set("angleRad", angle);
  SetMeasured(out, "correlation", result.correlation);
  out.Set("averageAbsAngleRad", Napi::Number::New(env, result.average_abs_angle_rad));
  out.Set("maxRadius", Napi::Number::New(env, result.max_radius));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringSpectrum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object opts = info[2].As<Napi::Object>();
    n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    smooth = BoolProperty(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = IntProperty(opts, "octaveFraction", kZeroIsSentinel);
    db_ref = FloatProperty(opts, "dbRef", 0.0f);
    db_amin = FloatProperty(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err = sonare_metering_spectrum(sonare_node::ElementData(typed), typed.ElementLength(),
                                             sr, n_fft, smooth, octave, db_ref, db_amin, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  return EmitSpectrumResult(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringSpectrumFrame(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  size_t frame_offset = 0;
  if (!OptionalSizeTArg(env, info, 2, "frameOffset", 0, &frame_offset)) return env.Undefined();
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 4 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    smooth = BoolProperty(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = IntProperty(opts, "octaveFraction", kZeroIsSentinel);
    db_ref = FloatProperty(opts, "dbRef", 0.0f);
    db_amin = FloatProperty(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err =
      sonare_metering_spectrum_frame(sonare_node::ElementData(typed), typed.ElementLength(), sr,
                                     frame_offset, n_fft, smooth, octave, db_ref, db_amin, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  return EmitSpectrumResult(env, result);
  SONARE_NODE_CATCH(env)
}

// --- Handle-form metering instance methods ---
//
// Each reads audio_ directly instead of a caller Float32Array, skipping the
// finiteness scan and defensive copy the buffer-form functions above pay per
// call. Parameters and defaults mirror the buffer form; see it for the
// contract. audio_ already passed validation at construction, so mutating the
// source buffer afterwards changes nothing these methods report.

Napi::Value SonareWrap::PeakDbInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float out_db = 0.0f;
  SonareError err = sonare_audio_peak_db(audio_, &out_db);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsDbInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float out_db = 0.0f;
  SonareError err = sonare_audio_rms_db(audio_, &out_db);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DcOffsetInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float out_value = 0.0f;
  SonareError err = sonare_audio_dc_offset(audio_, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CrestFactorDbInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float out_db = 0.0f;
  SonareError err = sonare_audio_crest_factor_db(audio_, &out_db);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return MeasuredNumber(env, out_db);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SilenceRatioInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float threshold_db{};
  if (!OptionalFloatArg(env, info, 0, "thresholdDb", -45.0f, &threshold_db)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 1, "frameLength", 1024, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 2, "hopLength", 256, &hop_length)) return env.Undefined();
  float ratio = 0.0f;
  const SonareError err =
      sonare_audio_silence_ratio(audio_, threshold_db, frame_length, hop_length, &ratio);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, ratio);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TruePeakDbInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  int oversample{};
  if (!OptionalIntArg(env, info, 0, "oversampleFactor", 4, &oversample)) return env.Undefined();
  float out_value = 0.0f;
  SonareError err = sonare_audio_true_peak_dbtp(audio_, oversample, &out_value);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectClippingInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  // Absent is the C ABI's NaN, the library's clip level; a written NaN is refused.
  const float threshold =
      DomainProperty(opts, "thresholdDb", std::numeric_limits<float>::quiet_NaN(),
                     sonare::metering::kClippingThresholdDbDomain);
  const int64_t min_region_value = Int64Property(opts, "minRegionSamples", 1);
  if (min_region_value < 0) {
    Napi::RangeError::New(env, "minRegionSamples must be an integer of at least 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t min_region = static_cast<size_t>(min_region_value);
  SonareClippingResult result{};
  SonareError err = sonare_audio_detect_clipping(audio_, threshold, min_region, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareClippingResult, sonare_free_clipping_result> guard(&result);
  Napi::Array regions = Napi::Array::New(env, result.region_count);
  for (size_t i = 0; i < result.region_count; ++i) {
    Napi::Object region = Napi::Object::New(env);
    region.Set("startSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].start_sample)));
    region.Set("endSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].end_sample)));
    region.Set("length", Napi::Number::New(env, static_cast<double>(result.regions[i].length)));
    region.Set("peak", Napi::Number::New(env, result.regions[i].peak));
    regions.Set(static_cast<uint32_t>(i), region);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("clippedSamples", Napi::Number::New(env, static_cast<double>(result.clipped_samples)));
  out.Set("clippingRatio", Napi::Number::New(env, result.clipping_ratio));
  out.Set("maxClippedPeak", Napi::Number::New(env, result.max_clipped_peak));
  out.Set("regions", regions);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DynamicRangeInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float window_sec = 0.0f;
  float hop_sec = 0.0f;
  // A NEGATIVE percentile selects the library default; 0.0f is a literal 0th
  // percentile request, so these read as plain fallbacks rather than
  // ZeroIsSentinel.
  float low_p = -1.0f;
  float high_p = -1.0f;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object opts = info[0].As<Napi::Object>();
    window_sec = FloatProperty(opts, "windowSec", window_sec);
    hop_sec = FloatProperty(opts, "hopSec", hop_sec);
    low_p = FloatProperty(opts, "lowPercentile", low_p);
    high_p = FloatProperty(opts, "highPercentile", high_p);
  }
  SonareDynamicRangeResult result{};
  SonareError err = sonare_audio_dynamic_range(audio_, window_sec, hop_sec, low_p, high_p, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareDynamicRangeResult, sonare_free_dynamic_range_result> guard(&result);
  auto windows = Float32ArrayFrom(env, result.window_rms_db, result.window_count);
  Napi::Object out = Napi::Object::New(env);
  SetMeasured(out, "dynamicRangeDb", result.dynamic_range_db);
  SetMeasured(out, "lowPercentileDb", result.low_percentile_db);
  SetMeasured(out, "highPercentileDb", result.high_percentile_db);
  out.Set("windowRmsDb", windows);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectrumInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object opts = info[0].As<Napi::Object>();
    n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    smooth = BoolProperty(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = IntProperty(opts, "octaveFraction", kZeroIsSentinel);
    db_ref = FloatProperty(opts, "dbRef", 0.0f);
    db_amin = FloatProperty(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err = sonare_audio_spectrum(audio_, n_fft, smooth, octave, db_ref, db_amin, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  return EmitSpectrumResult(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectrumFrameInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  size_t frame_offset = 0;
  if (!OptionalSizeTArg(env, info, 0, "frameOffset", 0, &frame_offset)) return env.Undefined();
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 2 && info[1].IsObject()) {
    Napi::Object opts = info[1].As<Napi::Object>();
    n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    smooth = BoolProperty(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = IntProperty(opts, "octaveFraction", kZeroIsSentinel);
    db_ref = FloatProperty(opts, "dbRef", 0.0f);
    db_amin = FloatProperty(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err = sonare_audio_spectrum_frame(audio_, frame_offset, n_fft, smooth, octave, db_ref,
                                                db_amin, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  return EmitSpectrumResult(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Ebur128LoudnessRangeLuInstance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!audio_) {
    sonare_node::ThrowHandleClosed(env, "Audio");
    return env.Undefined();
  }
  float out_lra = 0.0f;
  SonareError err = sonare_audio_ebur128_loudness_range_lu(audio_, &out_lra);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return MeasuredNumber(env, out_lra);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::WaveformPeaks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  int channels{};
  if (!RequiredIntArg(env, info, 1, "channels", &channels)) return env.Undefined();
  int64_t samples_per_bucket_value{};
  if (!RequiredDomainArg(env, info, 2, "samplesPerBucket",
                         sonare::metering::kSamplesPerBucketDomain, &samples_per_bucket_value)) {
    return env.Undefined();
  }
  const size_t samples_per_bucket = static_cast<size_t>(samples_per_bucket_value);
  if (channels <= 0 || typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "samples length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareWaveformPeaksResult result{};
  const size_t frames = typed.ElementLength() / static_cast<size_t>(channels);
  SonareError err = sonare_waveform_peaks(sonare_node::ElementData(typed), frames, channels,
                                          samples_per_bucket, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareWaveformPeaksResult, sonare_free_waveform_peaks_result> guard(&result);
  return EmitWaveformPeaksResult(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::WaveformPeakPyramid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  int channels{};
  if (!RequiredIntArg(env, info, 1, "channels", &channels)) return env.Undefined();
  if (!info[2].IsArray()) {
    Napi::TypeError::New(env, "levels must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  if (channels <= 0 || typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "samples length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array js_levels = info[2].As<Napi::Array>();
  std::vector<size_t> levels;
  const uint32_t level_count = node_list_length(js_levels, "levels");
  levels.reserve(level_count);
  for (uint32_t i = 0; i < level_count; ++i) {
    Napi::Value item = js_levels.Get(i);
    if (!item.IsNumber()) {
      Napi::TypeError::New(env, "levels must be numbers").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    levels.push_back(static_cast<size_t>(node_domain_number<int64_t>(
        item, ("samplesPerBucketLevels[" + std::to_string(i) + "]").c_str(),
        sonare::metering::kSamplesPerBucketDomain)));
  }
  SonareWaveformPeakPyramidResult result{};
  const size_t frames = typed.ElementLength() / static_cast<size_t>(channels);
  SonareError err = sonare_waveform_peak_pyramid(sonare_node::ElementData(typed), frames, channels,
                                                 levels.data(), levels.size(), &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  CResultGuard<SonareWaveformPeakPyramidResult, sonare_free_waveform_peak_pyramid_result> guard(
      &result);
  Napi::Array out = Napi::Array::New(env, result.level_count);
  for (size_t i = 0; i < result.level_count; ++i) {
    out.Set(i, EmitWaveformPeaksResult(env, result.levels[i]));
  }
  return out;
  SONARE_NODE_CATCH(env)
}
