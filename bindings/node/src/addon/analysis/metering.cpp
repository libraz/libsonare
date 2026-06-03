#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

int int_option(const Napi::Object& object, const char* key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

float float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().FloatValue() : fallback;
}

bool bool_option(const Napi::Object& object, const char* key, bool fallback) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
}

}  // namespace

Napi::Value SonareWrap::Lufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  SonareLufsResult lufs{};
  SonareError err = sonare_lufs(typed.Data(), typed.ElementLength(), sr, &lufs);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  result.Set("momentaryLufs", Napi::Number::New(env, lufs.momentary_lufs));
  result.Set("shortTermLufs", Napi::Number::New(env, lufs.short_term_lufs));
  result.Set("loudnessRange", Napi::Number::New(env, lufs.loudness_range));
  return result;
}

Napi::Value SonareWrap::MomentaryLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_momentary_lufs(typed.Data(), typed.ElementLength(), sr, &out, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, count * sizeof(float));
  }
  sonare_free_floats(out);
  return result;
}

Napi::Value SonareWrap::ShortTermLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_short_term_lufs(typed.Data(), typed.ElementLength(), sr, &out, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, count * sizeof(float));
  }
  sonare_free_floats(out);
  return result;
}

Napi::Value SonareWrap::LufsInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, channels, sampleRate?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int channels = info[1].As<Napi::Number>().Int32Value();
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  if (channels <= 0) {
    Napi::TypeError::New(env, "channels must be > 0").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::TypeError::New(env, "interleaved length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t frames = typed.ElementLength() / static_cast<size_t>(channels);

  SonareLufsResult lufs{};
  SonareError err = sonare_lufs_interleaved(typed.Data(), frames, channels, sr, &lufs);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  result.Set("momentaryLufs", Napi::Number::New(env, lufs.momentary_lufs));
  result.Set("shortTermLufs", Napi::Number::New(env, lufs.short_term_lufs));
  result.Set("loudnessRange", Napi::Number::New(env, lufs.loudness_range));
  return result;
}

Napi::Value SonareWrap::Ebur128LoudnessRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float out_lra = 0.0f;
  SonareError err =
      sonare_ebur128_loudness_range(typed.Data(), typed.ElementLength(), sr, &out_lra);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_lra);
}

namespace {

// Shared shape for the scalar offline meters (peak/rms/crest/dc).
using MeteringScalarFn = SonareError (*)(const float*, size_t, int, float*);

Napi::Value MeteringScalar(const Napi::CallbackInfo& info, MeteringScalarFn fn,
                           const char* fn_label) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, std::string(fn_label) + ": expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float out_value = 0.0f;
  SonareError err = fn(typed.Data(), typed.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringPeakDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_peak_db, "meteringPeakDb");
}

Napi::Value SonareWrap::MeteringRmsDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_rms_db, "meteringRmsDb");
}

Napi::Value SonareWrap::MeteringCrestFactorDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_crest_factor_db, "meteringCrestFactorDb");
}

Napi::Value SonareWrap::MeteringDcOffset(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_dc_offset, "meteringDcOffset");
}

Napi::Value SonareWrap::MeteringTruePeakDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringTruePeakDb: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int oversample =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 4;
  float out_value = 0.0f;
  SonareError err =
      sonare_metering_true_peak_db(typed.Data(), typed.ElementLength(), sr, oversample, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::MeteringDetectClipping(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringDetectClipping: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float threshold =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.999f;
  size_t min_region = info.Length() >= 4 && info[3].IsNumber()
                          ? static_cast<size_t>(info[3].As<Napi::Number>().Uint32Value())
                          : 1u;
  SonareClippingResult result{};
  SonareError err = sonare_metering_detect_clipping(typed.Data(), typed.ElementLength(), sr,
                                                    threshold, min_region, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
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
  sonare_free_clipping_result(&result);
  return out;
}

Napi::Value SonareWrap::MeteringDynamicRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringDynamicRange: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float window_sec =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float hop_sec =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  float low_p =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.0f;
  float high_p =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 0.0f;
  SonareDynamicRangeResult result{};
  SonareError err = sonare_metering_dynamic_range(typed.Data(), typed.ElementLength(), sr,
                                                  window_sec, hop_sec, low_p, high_p, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto windows = Napi::Float32Array::New(env, result.window_count);
  if (result.window_count > 0 && result.window_rms_db != nullptr) {
    std::memcpy(windows.Data(), result.window_rms_db, result.window_count * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("dynamicRangeDb", Napi::Number::New(env, result.dynamic_range_db));
  out.Set("lowPercentileDb", Napi::Number::New(env, result.low_percentile_db));
  out.Set("highPercentileDb", Napi::Number::New(env, result.high_percentile_db));
  out.Set("windowRmsDb", windows);
  sonare_free_dynamic_range_result(&result);
  return out;
}

namespace {

bool ParseScaleArgs(const Napi::CallbackInfo& info, int* root, uint16_t* mode_mask,
                    float* reference_midi, float* midi) {
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    return false;
  }
  *root = info[0].As<Napi::Number>().Int32Value();
  int mask_int = info[1].As<Napi::Number>().Int32Value();
  // modeMask is a 12-bit pitch-class set (one bit per semitone). Validate the
  // range explicitly: the narrowing cast to uint16_t would otherwise turn -1
  // into 0xFFFF and any value > 4095 into an unrelated mask.
  if (mask_int < 0 || mask_int > 4095) {
    Napi::RangeError::New(info.Env(), "modeMask must be in [0, 4095]").ThrowAsJavaScriptException();
    return false;
  }
  *mode_mask = static_cast<uint16_t>(mask_int);
  *midi = info[2].As<Napi::Number>().FloatValue();
  *reference_midi =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  return true;
}

}  // namespace

Napi::Value SonareWrap::ScaleQuantizeMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) {
    if (env.IsExceptionPending()) return env.Undefined();
    Napi::TypeError::New(env, "scaleQuantizeMidi: expected (root, modeMask, midi, referenceMidi?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float out_value = 0.0f;
  SonareError err = sonare_scale_quantize_midi(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::ScaleCorrectionSemitones(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) {
    if (env.IsExceptionPending()) return env.Undefined();
    Napi::TypeError::New(
        env, "scaleCorrectionSemitones: expected (root, modeMask, midi, referenceMidi?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float out_value = 0.0f;
  SonareError err = sonare_scale_correction_semitones(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::ScalePitchClassEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "scalePitchClassEnabled: expected (root, modeMask, pitchClass)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int root = info[0].As<Napi::Number>().Int32Value();
  uint16_t mask = static_cast<uint16_t>(info[1].As<Napi::Number>().Int32Value());
  int pitch_class = info[2].As<Napi::Number>().Int32Value();
  int out_enabled = 0;
  SonareError err = sonare_scale_pitch_class_enabled(root, mask, pitch_class, &out_enabled);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Boolean::New(env, out_enabled != 0);
}

namespace {

using StereoScalarFn = SonareError (*)(const float*, const float*, size_t, int, float*);

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

Napi::Value StereoScalar(const Napi::CallbackInfo& info, StereoScalarFn fn, const char* fn_label) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(
        env, std::string(fn_label) + ": expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, std::string(fn_label) + ": left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  float out_value = 0.0f;
  SonareError err = fn(left.Data(), right.Data(), left.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringStereoCorrelation(const Napi::CallbackInfo& info) {
  return StereoScalar(info, &sonare_metering_stereo_correlation, "meteringStereoCorrelation");
}

Napi::Value SonareWrap::MeteringStereoWidth(const Napi::CallbackInfo& info) {
  return StereoScalar(info, &sonare_metering_stereo_width, "meteringStereoWidth");
}

Napi::Value SonareWrap::MeteringVectorscope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env,
                         "meteringVectorscope: expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "meteringVectorscope: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  SonareVectorscopeResult result{};
  SonareError err =
      sonare_metering_vectorscope(left.Data(), right.Data(), left.ElementLength(), sr, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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
}

Napi::Value SonareWrap::MeteringPhaseScope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env,
                         "meteringPhaseScope: expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "meteringPhaseScope: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  SonarePhaseScopeResult result{};
  SonareError err =
      sonare_metering_phase_scope(left.Data(), right.Data(), left.ElementLength(), sr, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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
  out.Set("correlation", Napi::Number::New(env, result.correlation));
  out.Set("averageAbsAngleRad", Napi::Number::New(env, result.average_abs_angle_rad));
  out.Set("maxRadius", Napi::Number::New(env, result.max_radius));
  return out;
}

Napi::Value SonareWrap::MeteringSpectrum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringSpectrum: expected Float32Array samples")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object opts = info[2].As<Napi::Object>();
    n_fft = int_option(opts, "nFft", 0);
    smooth = bool_option(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = int_option(opts, "octaveFraction", 0);
    db_ref = float_option(opts, "dbRef", 0.0f);
    db_amin = float_option(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err = sonare_metering_spectrum(typed.Data(), typed.ElementLength(), sr, n_fft, smooth,
                                             octave, db_ref, db_amin, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  const size_t bytes = result.bin_count * sizeof(float);
  auto freq = Napi::Float32Array::New(env, result.bin_count);
  auto mag = Napi::Float32Array::New(env, result.bin_count);
  auto pwr = Napi::Float32Array::New(env, result.bin_count);
  auto db = Napi::Float32Array::New(env, result.bin_count);
  if (result.bin_count > 0) {
    std::memcpy(freq.Data(), result.frequencies, bytes);
    std::memcpy(mag.Data(), result.magnitude, bytes);
    std::memcpy(pwr.Data(), result.power, bytes);
    std::memcpy(db.Data(), result.db, bytes);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("frequencies", freq);
  out.Set("magnitude", mag);
  out.Set("power", pwr);
  out.Set("db", db);
  out.Set("nFft", Napi::Number::New(env, result.n_fft));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  return out;
}
