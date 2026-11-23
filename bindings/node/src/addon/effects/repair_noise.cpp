/// @file
/// @brief Node bindings for the broadband and tonal noise repairs: denoise, dehum.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "c_api/mastering_input_domains.h"
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
  using sonare::mastering::repair::DenoiseMode;
  static constexpr NodeChoice<DenoiseMode> kChoices[] = {
      {"logmmse", DenoiseMode::LogMmse},
      {"log_mmse", DenoiseMode::LogMmse},
      {"lsa", DenoiseMode::LogMmse},
      {"mmsestsa", DenoiseMode::MmseStsa},
      {"mmse_stsa", DenoiseMode::MmseStsa},
      {"stsa", DenoiseMode::MmseStsa},
      {"spectralsubtraction", DenoiseMode::SpectralSubtraction},
      {"spectral_subtraction", DenoiseMode::SpectralSubtraction},
      {"ss", DenoiseMode::SpectralSubtraction}};
  return ChoiceProperty(options, "mode", kChoices, fallback);
}

sonare::mastering::repair::DenoiseNoiseEstimator parse_denoise_noise_estimator(
    const Napi::Object& options, sonare::mastering::repair::DenoiseNoiseEstimator fallback) {
  using sonare::mastering::repair::DenoiseNoiseEstimator;
  static constexpr NodeChoice<DenoiseNoiseEstimator> kChoices[] = {
      {"quantile", DenoiseNoiseEstimator::Quantile},
      {"mcra", DenoiseNoiseEstimator::Mcra},
      {"imcra", DenoiseNoiseEstimator::Imcra},
      {"spp", DenoiseNoiseEstimator::Spp}};
  return ChoiceProperty(options, "noiseEstimator", kChoices, fallback);
}

int parse_dehum_mode(const Napi::Object& options, int fallback) {
  static constexpr NodeChoice<int> kChoices[] = {{"subtract", SONARE_DEHUM_MODE_SUBTRACT},
                                                 {"notch", SONARE_DEHUM_MODE_NOTCH}};
  return ChoiceProperty(options, "mode", kChoices, fallback);
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
SonareDenoiseClassicalConfig DefaultDenoiseConfig() {
  SonareDenoiseClassicalConfig config{};
  sonare_denoise_classical_config_init(&config);
  return config;
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
SonareDehumConfig DefaultDehumConfig() {
  SonareDehumConfig config{};
  sonare_dehum_config_init(&config);
  return config;
}

/// @brief Marshal a noise analysis into the JS shape the denoise reports carry: the measured
///        bands' levels beside each one's grid index.
Napi::Object EmitNoiseDetection(Napi::Env env, float floor_dbfs,
                                const std::vector<std::pair<int32_t, float>>& bands) {
  Napi::Object out = Napi::Object::New(env);
  SetMeasured(out, "floorDbfs", floor_dbfs);
  Napi::Array band_floor_dbfs = Napi::Array::New(env, bands.size());
  Napi::Array band_floor_band = Napi::Array::New(env, bands.size());
  for (uint32_t i = 0; i < bands.size(); ++i) {
    band_floor_band.Set(i, Napi::Number::New(env, bands[i].first));
    band_floor_dbfs.Set(i, Napi::Number::New(env, bands[i].second));
  }
  out.Set("bandFloorDbfs", band_floor_dbfs);
  out.Set("bandFloorBand", band_floor_band);
  return out;
}

/// @brief The C record packs the measured bands into [0, band_count) and names each band.
Napi::Object EmitNoiseDetection(Napi::Env env, const SonareNoiseDetection& detection) {
  std::vector<std::pair<int32_t, float>> bands;
  bands.reserve(detection.band_count);
  for (size_t i = 0; i < detection.band_count; ++i) {
    bands.emplace_back(detection.band_floor_band[i], detection.band_floor_dbfs[i]);
  }
  return EmitNoiseDetection(env, detection.floor_dbfs, bands);
}

/// @brief The core record holds every band; one with no bin (NaN) is left out with its index.
Napi::Object EmitNoiseDetection(Napi::Env env,
                                const sonare::mastering::repair::NoiseDetection& detection) {
  std::vector<std::pair<int32_t, float>> bands;
  for (int32_t i = 0; i < SONARE_REPAIR_NOISE_BAND_COUNT; ++i) {
    if (std::isnan(detection.band_floor_dbfs[i])) continue;
    bands.emplace_back(i, detection.band_floor_dbfs[i]);
  }
  return EmitNoiseDetection(env, detection.floor_dbfs, bands);
}

template <typename T>
Napi::Object EmitDenoiseReport(Napi::Env env, const T& report) {
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

/// @brief Marshal one channel's hum detection into the JS shape shared by the
///        dehum stereo report.
template <typename T>
Napi::Object EmitHumDetection(Napi::Env env, const T& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("fundamentalHz", Napi::Number::New(env, detection.fundamental_hz));
  out.Set("fundamentalProminence", Napi::Number::New(env, detection.fundamental_prominence));
  out.Set("harmonics", Napi::Number::New(env, detection.harmonics));
  // The measured harmonics are a prefix; the unmeasured (NaN) suffix past Nyquist is cut.
  Napi::Array harmonic_dbfs = Napi::Array::New(env);
  for (int i = 0; i < SONARE_DEHUM_MAX_HARMONICS && !std::isnan(detection.harmonic_dbfs[i]); ++i) {
    harmonic_dbfs.Set(static_cast<uint32_t>(i), Napi::Number::New(env, detection.harmonic_dbfs[i]));
  }
  out.Set("harmonicDbfs", harmonic_dbfs);
  return out;
}

template <typename T>
Napi::Object EmitDehumReport(Napi::Env env, const T& report) {
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

}  // namespace

Napi::Value SonareWrap::MasteringRepairDenoiseClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireMonoArgs(info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", sonare_node::ElementData(typed),
                                       typed.ElementLength(), sr);
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
        FloatProperty(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain =
        BoolProperty(options, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = BoolProperty(options, "gainSmoothing", config.gain_smoothing);
  }
  if (config.hop_length <= 0) {
    Napi::RangeError::New(env, "hopLength must be an integer in [1, " + node_int_max_text() + "]")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio =
      sonare::Audio::from_buffer(sonare_node::ElementData(typed), typed.ElementLength(), sr);
  sonare::mastering::repair::DenoiseReport report;
  sonare::Audio result = sonare::mastering::repair::denoise_classical(audio, config, &report);
  return EmitMonoResult(env, result, EmitDenoiseReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDenoiseClassicalConfig), applied
  // before any options key overrides a field.
  SonareDenoiseClassicalConfig config{};
  sonare_denoise_classical_config_init(&config);
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_denoise_config(info[3].As<Napi::Object>(), config);
  }
  SonareDenoiseStereoResult result{};
  SonareError err = sonare_mastering_repair_denoise_classical_stereo(
      sonare_node::ElementData(left), sonare_node::ElementData(right), left.ElementLength(), sr,
      &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareDenoiseStereoResult> guard(&result);
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("report", EmitDenoiseReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!CheckLinkedArgs(env, info)) return env.Undefined();
  ChannelInputs inputs;
  if (!ReadChannelInputs(env, info[0], &inputs)) return env.Undefined();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = DefaultDenoiseConfig();
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_denoise_config(info[2].As<Napi::Object>(), config);
  }
  SonareDenoiseLinkedResult result{};
  SonareError err = sonare_mastering_repair_denoise_classical_linked(
      sonare_node::ElementData(inputs.ptrs), inputs.ptrs.size(), inputs.length, sr, &config,
      &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  LinkedResultGuard<SonareDenoiseLinkedResult> guard(&result);
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels",
          EmitLinkedResultChannels(env, result.channels, result.num_channels, result.length));
  out.Set("report", EmitDenoiseReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectNoiseFloor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireMonoArgs(info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = DefaultDenoiseConfig();
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_denoise_config(info[2].As<Napi::Object>(), config);
  SonareNoiseDetection detection{};
  SonareError err = sonare_mastering_repair_detect_noise_floor(
      sonare_node::ElementData(typed), typed.ElementLength(), sr, has_options ? &config : nullptr,
      &detection);
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
  // nFft is optional (an omitted request reads as an empty one); sampleRate is not.
  Napi::Object request = info.Length() >= 1 && info[0].IsObject() ? info[0].As<Napi::Object>()
                                                                  : Napi::Object::New(env);
  const int n_fft = IntProperty(request, "nFft", DefaultDenoiseConfig().n_fft);
  const Napi::Value rate = request.Get("sampleRate");
  node_require_property_type(env, rate.IsNumber(), "sampleRate", "an integer");
  const int sample_rate =
      node_domain_number<int>(rate, "sampleRate", sonare_c_detail::kNoiseBandSampleRateDomain);
  // The C entry writes into the array's own storage; no intermediate buffer.
  auto bins = Napi::Int32Array::New(env, SONARE_REPAIR_NOISE_BAND_EDGE_COUNT);
  SonareError err =
      sonare_mastering_repair_noise_band_bins(n_fft, sample_rate, sonare_node::ElementData(bins));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return bins;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireMonoArgs(info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", sonare_node::ElementData(typed),
                                       typed.ElementLength(), sr);
  sonare::mastering::repair::DehumConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.fundamental_hz = FloatProperty(options, "fundamentalHz", config.fundamental_hz);
    config.harmonics = IntProperty(options, "harmonics", config.harmonics);
    config.q = FloatProperty(options, "q", config.q);
    config.adaptive = BoolProperty(options, "adaptive", config.adaptive);
    config.search_range_hz = FloatProperty(options, "searchRangeHz", config.search_range_hz);
    config.adaptation = FloatProperty(options, "adaptation", config.adaptation);
    config.frame_size = IntProperty(options, "frameSize", config.frame_size);
    config.pll_bandwidth = FloatProperty(options, "pllBandwidth", config.pll_bandwidth);
    config.mode = static_cast<sonare::mastering::repair::DehumMode>(
        parse_dehum_mode(options, static_cast<int>(config.mode)));
  }
  sonare::Audio audio =
      sonare::Audio::from_buffer(sonare_node::ElementData(typed), typed.ElementLength(), sr);
  sonare::mastering::repair::DehumReport report;
  sonare::Audio result = sonare::mastering::repair::dehum(audio, config, &report);
  return EmitMonoResult(env, result, EmitDehumReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehumStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDehumConfig), applied before
  // any options key overrides a field.
  SonareDehumConfig config{};
  sonare_dehum_config_init(&config);
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_dehum_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDehumStereoResult result{};
  SonareError err = sonare_mastering_repair_dehum_stereo(
      sonare_node::ElementData(left), sonare_node::ElementData(right), left.ElementLength(), sr,
      &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareDehumStereoResult> guard(&result);
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("leftReport", EmitDehumReport(env, result.left_report));
  out.Set("rightReport", EmitDehumReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectHum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireMonoArgs(info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDehumConfig config = DefaultDehumConfig();
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_dehum_config_c(info[2].As<Napi::Object>(), config);
  SonareHumDetection detection{};
  SonareError err =
      sonare_mastering_repair_detect_hum(sonare_node::ElementData(typed), typed.ElementLength(), sr,
                                         has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitHumDetection(env, detection);
  SONARE_NODE_CATCH(env)
}
