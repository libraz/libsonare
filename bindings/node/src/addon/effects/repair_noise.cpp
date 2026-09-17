/// @file
/// @brief Node bindings for the broadband and tonal noise repairs: denoise, dehum.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/audio.h"
#include "effects/repair_common.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::repair_detail;

namespace {

sonare::mastering::repair::DenoiseMode parse_denoise_mode(
    const Napi::Object& options, sonare::mastering::repair::DenoiseMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return sonare::mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return sonare::mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw std::runtime_error("unknown denoise mode: " + value.As<Napi::String>().Utf8Value());
}

sonare::mastering::repair::DenoiseNoiseEstimator parse_denoise_noise_estimator(
    const Napi::Object& options, sonare::mastering::repair::DenoiseNoiseEstimator fallback) {
  Napi::Value value = options.Get("noiseEstimator");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise noise estimator must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return sonare::mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
  if (s == "spp") return sonare::mastering::repair::DenoiseNoiseEstimator::Spp;
  throw std::runtime_error("unknown denoise noise estimator: " +
                           value.As<Napi::String>().Utf8Value());
}

int parse_dehum_mode(const Napi::Object& options, int fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("dehum mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "subtract") return SONARE_DEHUM_MODE_SUBTRACT;
  if (s == "notch") return SONARE_DEHUM_MODE_NOTCH;
  throw std::runtime_error("unknown dehum mode: " + value.As<Napi::String>().Utf8Value());
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareDenoiseClassicalConfig kDenoiseConfigDefaults{
    SONARE_DENOISE_MODE_LOG_MMSE,
    SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
    1024,
    256,
    0.98f,
    26.0f,
    2.0f,
    0.05f,
    0.1f,
    1,
    1};

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareDehumConfig kDehumConfigDefaults{
    50.0f, 4, 20.0f, 0, 2.0f, 0.25f, 2048, 0.01f, SONARE_DEHUM_MODE_SUBTRACT};

/// @brief Marshal the pair-level noise analysis into the JS shape the denoise
///        stereo report carries.
Napi::Object EmitNoiseDetection(Napi::Env env, const SonareNoiseDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("floorDbfs", Napi::Number::New(env, detection.floor_dbfs));
  Napi::Array band_floor_dbfs = Napi::Array::New(env, SONARE_REPAIR_NOISE_BAND_COUNT);
  for (int i = 0; i < SONARE_REPAIR_NOISE_BAND_COUNT; ++i) {
    band_floor_dbfs.Set(static_cast<uint32_t>(i),
                        Napi::Number::New(env, detection.band_floor_dbfs[i]));
  }
  out.Set("bandFloorDbfs", band_floor_dbfs);
  return out;
}

Napi::Object EmitDenoiseReport(Napi::Env env, const SonareDenoiseReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitNoiseDetection(env, report.detected));
  out.Set("meanReductionDb", Napi::Number::New(env, report.mean_reduction_db));
  out.Set("maxReductionDb", Napi::Number::New(env, report.max_reduction_db));
  out.Set("floorLimitedFraction", Napi::Number::New(env, report.floor_limited_fraction));
  return out;
}

/// @brief Read a SonareDenoiseClassicalConfig options bag, reusing the mono
///        facade's own mode and estimator string readers above so the two paths
///        cannot recognize different spellings of the same mode.
SonareDenoiseClassicalConfig read_denoise_config(const Napi::Object& options,
                                                 SonareDenoiseClassicalConfig config) {
  config.mode = static_cast<int>(parse_denoise_mode(
      options, static_cast<sonare::mastering::repair::DenoiseMode>(config.mode)));
  config.noise_estimator = static_cast<int>(parse_denoise_noise_estimator(
      options,
      static_cast<sonare::mastering::repair::DenoiseNoiseEstimator>(config.noise_estimator)));
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.dd_alpha = FloatProperty(options, "ddAlpha", config.dd_alpha);
  config.reduction_db = FloatProperty(options, "reductionDb", config.reduction_db);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.noise_estimation_quantile =
      FloatProperty(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
  config.speech_presence_gain =
      BoolProperty(options, "speechPresenceGain", config.speech_presence_gain != 0) ? 1 : 0;
  config.gain_smoothing =
      BoolProperty(options, "gainSmoothing", config.gain_smoothing != 0) ? 1 : 0;
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDenoiseStereoResult on scope
///        exit -- mirrors DeclickStereoResultGuard above.
class DenoiseStereoResultGuard {
 public:
  explicit DenoiseStereoResultGuard(SonareDenoiseStereoResult* result) : result_(result) {}
  DenoiseStereoResultGuard(const DenoiseStereoResultGuard&) = delete;
  DenoiseStereoResultGuard& operator=(const DenoiseStereoResultGuard&) = delete;
  ~DenoiseStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDenoiseStereoResult* result_;
};

/// @brief Marshal one channel's hum detection into the JS shape shared by the
///        dehum stereo report.
Napi::Object EmitHumDetection(Napi::Env env, const SonareHumDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("fundamentalHz", Napi::Number::New(env, detection.fundamental_hz));
  out.Set("fundamentalProminence", Napi::Number::New(env, detection.fundamental_prominence));
  out.Set("harmonics", Napi::Number::New(env, detection.harmonics));
  Napi::Array harmonic_dbfs = Napi::Array::New(env, SONARE_DEHUM_MAX_HARMONICS);
  for (int i = 0; i < SONARE_DEHUM_MAX_HARMONICS; ++i) {
    harmonic_dbfs.Set(static_cast<uint32_t>(i), Napi::Number::New(env, detection.harmonic_dbfs[i]));
  }
  out.Set("harmonicDbfs", harmonic_dbfs);
  return out;
}

Napi::Object EmitDehumReport(Napi::Env env, const SonareDehumReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitHumDetection(env, report.detected));
  out.Set("notchedHarmonics", Napi::Number::New(env, report.notched_harmonics));
  out.Set("appliedFundamentalHz", Napi::Number::New(env, report.applied_fundamental_hz));
  out.Set("fundamentalDriftHz", Napi::Number::New(env, report.fundamental_drift_hz));
  return out;
}

/// @brief Read a SonareDehumConfig options bag, applying the same field names
///        and defaults as the mono facade's DehumConfig mapping above -- the
///        two structs share their shape, this is the C-ABI mirror of that
///        reading.
SonareDehumConfig read_dehum_config_c(const Napi::Object& options, SonareDehumConfig config) {
  config.fundamental_hz = FloatProperty(options, "fundamentalHz", config.fundamental_hz);
  config.harmonics = IntProperty(options, "harmonics", config.harmonics);
  config.q = FloatProperty(options, "q", config.q);
  config.adaptive = BoolProperty(options, "adaptive", config.adaptive != 0) ? 1 : 0;
  config.search_range_hz = FloatProperty(options, "searchRangeHz", config.search_range_hz);
  config.adaptation = FloatProperty(options, "adaptation", config.adaptation);
  config.frame_size = IntProperty(options, "frameSize", config.frame_size);
  config.pll_bandwidth = FloatProperty(options, "pllBandwidth", config.pll_bandwidth);
  config.mode = parse_dehum_mode(options, config.mode);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDehumStereoResult on scope
///        exit -- mirrors DecrackleStereoResultGuard above.
class DehumStereoResultGuard {
 public:
  explicit DehumStereoResultGuard(SonareDehumStereoResult* result) : result_(result) {}
  DehumStereoResultGuard(const DehumStereoResultGuard&) = delete;
  DehumStereoResultGuard& operator=(const DehumStereoResultGuard&) = delete;
  ~DehumStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDehumStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDenoiseClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DenoiseClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.mode = parse_denoise_mode(options, config.mode);
    config.noise_estimator = parse_denoise_noise_estimator(options, config.noise_estimator);
    config.n_fft = IntProperty(options, "nFft", config.n_fft);
    config.hop_length = IntProperty(options, "hopLength", config.hop_length);
    config.dd_alpha = FloatProperty(options, "ddAlpha", config.dd_alpha);
    config.reduction_db = FloatProperty(options, "reductionDb", config.reduction_db);
    config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        node_float_option(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain =
        node_bool_option(options, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = node_bool_option(options, "gainSmoothing", config.gain_smoothing);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0) {
    Napi::RangeError::New(env, "hopLength must be positive").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::denoise_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(
        env, "masteringRepairDenoiseClassicalStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDenoiseClassicalConfig), applied
  // before any options key overrides a field.
  SonareDenoiseClassicalConfig config{SONARE_DENOISE_MODE_LOG_MMSE,
                                      SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
                                      1024,
                                      256,
                                      0.98f,
                                      26.0f,
                                      2.0f,
                                      0.05f,
                                      0.1f,
                                      1,
                                      1};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_denoise_config(info[3].As<Napi::Object>(), config);
  }
  SonareDenoiseStereoResult result{};
  SonareError err = sonare_mastering_repair_denoise_classical_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DenoiseStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitDenoiseReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckLinkedArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  LinkedPlanes planes;
  if (!ReadLinkedPlanes(env, info[0], "masteringRepairDenoiseClassicalLinked", &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = kDenoiseConfigDefaults;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_denoise_config(info[2].As<Napi::Object>(), config);
  }
  SonareDenoiseReport report{};
  SonareError err = sonare_mastering_repair_denoise_classical_linked(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, &config,
      planes.out_ptrs.data(), &report);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels", EmitLinkedChannels(env, planes));
  out.Set("report", EmitDenoiseReport(env, report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectNoiseFloor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = kDenoiseConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_denoise_config(info[2].As<Napi::Object>(), config);
  SonareNoiseDetection detection{};
  SonareError err = sonare_mastering_repair_detect_noise_floor(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitNoiseDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairNoiseBandBins(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  SONARE_NODE_TRY
  // Every field is optional, so an omitted request reads as an empty one.
  Napi::Object request = info.Length() >= 1 && info[0].IsObject() ? info[0].As<Napi::Object>()
                                                                  : Napi::Object::New(env);
  const int n_fft = IntProperty(request, "nFft", kDenoiseConfigDefaults.n_fft);
  const int sample_rate = IntProperty(request, "sampleRate", sonare::constants::kDefaultSampleRate);
  // The C entry writes into the array's own storage; no intermediate buffer.
  auto bins = Napi::Int32Array::New(env, SONARE_REPAIR_NOISE_BAND_EDGE_COUNT);
  SonareError err = sonare_mastering_repair_noise_band_bins(n_fft, sample_rate, bins.Data());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return bins;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DehumConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.fundamental_hz = node_float_option(options, "fundamentalHz", config.fundamental_hz);
    config.harmonics = node_int_option(options, "harmonics", config.harmonics);
    config.q = FloatProperty(options, "q", config.q);
    config.adaptive = node_bool_option(options, "adaptive", config.adaptive);
    config.search_range_hz = node_float_option(options, "searchRangeHz", config.search_range_hz);
    config.adaptation = node_float_option(options, "adaptation", config.adaptation);
    config.frame_size = node_int_option(options, "frameSize", config.frame_size);
    config.pll_bandwidth = node_float_option(options, "pllBandwidth", config.pll_bandwidth);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dehum(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehumStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "masteringRepairDehumStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDehumConfig), applied before
  // any options key overrides a field.
  SonareDehumConfig config{
      50.0f, 4, 20.0f, 0, 2.0f, 0.25f, 2048, 0.01f, SONARE_DEHUM_MODE_SUBTRACT};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_dehum_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDehumStereoResult result{};
  SonareError err = sonare_mastering_repair_dehum_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DehumStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("leftReport", EmitDehumReport(env, result.left_report));
  out.Set("rightReport", EmitDehumReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectHum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDehumConfig config = kDehumConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_dehum_config_c(info[2].As<Napi::Object>(), config);
  SonareHumDetection detection{};
  SonareError err = sonare_mastering_repair_detect_hum(typed.Data(), typed.ElementLength(), sr,
                                                       has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitHumDetection(env, detection);
  SONARE_NODE_CATCH(env)
}
