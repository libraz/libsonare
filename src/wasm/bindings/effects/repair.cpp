/// @file repair.cpp
/// @brief Embind bindings for offline mastering repair APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>

#include "wasm/bindings/common/common.h"

// ============================================================================
// Mastering — offline repair processors (declick / denoise_classical)
// ============================================================================

namespace {

// Match the Node repair bindings: missing, null, undefined, and values of the
// wrong primitive type retain the config default.  In particular, do not pass
// embind's undefined-to-NaN coercion on to integer conversions in DSP configs.
bool repairOptionValue(const val& options, const char* key, val* value) {
  if (!hasProperty(options, key)) return false;
  *value = options[key];
  return !value->isUndefined() && !value->isNull();
}

int repairIntOption(const val& options, const char* key, int fallback) {
  val value = val::undefined();
  return repairOptionValue(options, key, &value) && value.typeOf().as<std::string>() == "number"
             ? value.as<int>()
             : fallback;
}

float repairFloatOption(const val& options, const char* key, float fallback) {
  val value = val::undefined();
  return repairOptionValue(options, key, &value) && value.typeOf().as<std::string>() == "number"
             ? value.as<float>()
             : fallback;
}

bool repairBoolOption(const val& options, const char* key, bool fallback) {
  val value = val::undefined();
  return repairOptionValue(options, key, &value) && value.typeOf().as<std::string>() == "boolean"
             ? value.as<bool>()
             : fallback;
}

}  // namespace

val js_mastering_repair_declick(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    cfg.neighbor_ratio = repairFloatOption(options, "neighborRatio", cfg.neighbor_ratio);
    if (hasProperty(options, "maxClickSamples")) {
      const int v =
          repairIntOption(options, "maxClickSamples", static_cast<int>(cfg.max_click_samples));
      if (v <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringRepairDeclick: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.residual_ratio = repairFloatOption(options, "residualRatio", cfg.residual_ratio);
  }
  Audio result = mastering::repair::declick(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

mastering::repair::DenoiseMode parseDenoiseMode(const std::string& name,
                                                mastering::repair::DenoiseMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise mode: " + name);
}

mastering::repair::DenoiseNoiseEstimator parseDenoiseNoiseEstimator(
    const std::string& name, mastering::repair::DenoiseNoiseEstimator fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return mastering::repair::DenoiseNoiseEstimator::Imcra;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise noise estimator: " + name);
}

}  // namespace

val js_mastering_repair_denoise_classical(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDenoiseMode(value.as<std::string>(), cfg.mode);
      }
    }
    if (hasProperty(options, "noiseEstimator")) {
      val value = val::undefined();
      if (repairOptionValue(options, "noiseEstimator", &value)) {
        cfg.noise_estimator =
            parseDenoiseNoiseEstimator(value.as<std::string>(), cfg.noise_estimator);
      }
    }
    cfg.n_fft = repairIntOption(options, "nFft", cfg.n_fft);
    cfg.hop_length = repairIntOption(options, "hopLength", cfg.hop_length);
    cfg.dd_alpha = repairFloatOption(options, "ddAlpha", cfg.dd_alpha);
    cfg.gain_floor = repairFloatOption(options, "gainFloor", cfg.gain_floor);
    cfg.over_subtraction = repairFloatOption(options, "overSubtraction", cfg.over_subtraction);
    cfg.spectral_floor = repairFloatOption(options, "spectralFloor", cfg.spectral_floor);
    cfg.noise_estimation_quantile =
        repairFloatOption(options, "noiseEstimationQuantile", cfg.noise_estimation_quantile);
    cfg.speech_presence_gain =
        repairBoolOption(options, "speechPresenceGain", cfg.speech_presence_gain);
    cfg.gain_smoothing = repairBoolOption(options, "gainSmoothing", cfg.gain_smoothing);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairDenoiseClassical: hopLength must be positive");
  }
  Audio result = mastering::repair::denoise_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_declip(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.clip_threshold = repairFloatOption(options, "clipThreshold", cfg.clip_threshold);
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.iterations = repairIntOption(options, "iterations", cfg.iterations);
    cfg.lpc_blend = repairFloatOption(options, "lpcBlend", cfg.lpc_blend);
  }
  Audio result = mastering::repair::declip(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

mastering::repair::DecrackleMode parseDecrackleMode(const std::string& name,
                                                    mastering::repair::DecrackleMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "median") return mastering::repair::DecrackleMode::Median;
  if (s == "waveletshrinkage" || s == "wavelet_shrinkage" || s == "wavelet") {
    return mastering::repair::DecrackleMode::WaveletShrinkage;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown decrackle mode: " + name);
}

mastering::repair::TrimSilenceMode parseTrimSilenceMode(
    const std::string& name, mastering::repair::TrimSilenceMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown trim silence mode: " + name);
}

}  // namespace

val js_mastering_repair_decrackle(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDecrackleMode(value.as<std::string>(), cfg.mode);
      }
    }
    cfg.levels = repairIntOption(options, "levels", cfg.levels);
  }
  Audio result = mastering::repair::decrackle(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dehum(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.fundamental_hz = repairFloatOption(options, "fundamentalHz", cfg.fundamental_hz);
    cfg.harmonics = repairIntOption(options, "harmonics", cfg.harmonics);
    cfg.q = repairFloatOption(options, "q", cfg.q);
    cfg.adaptive = repairBoolOption(options, "adaptive", cfg.adaptive);
    cfg.search_range_hz = repairFloatOption(options, "searchRangeHz", cfg.search_range_hz);
    cfg.adaptation = repairFloatOption(options, "adaptation", cfg.adaptation);
    cfg.frame_size = repairIntOption(options, "frameSize", cfg.frame_size);
    cfg.pll_bandwidth = repairFloatOption(options, "pllBandwidth", cfg.pll_bandwidth);
  }
  Audio result = mastering::repair::dehum(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dereverb_classical(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    cfg.attenuation = repairFloatOption(options, "attenuation", cfg.attenuation);
    cfg.n_fft = repairIntOption(options, "nFft", cfg.n_fft);
    cfg.hop_length = repairIntOption(options, "hopLength", cfg.hop_length);
    cfg.t60_sec = repairFloatOption(options, "t60Sec", cfg.t60_sec);
    cfg.late_delay_ms = repairFloatOption(options, "lateDelayMs", cfg.late_delay_ms);
    cfg.over_subtraction = repairFloatOption(options, "overSubtraction", cfg.over_subtraction);
    cfg.spectral_floor = repairFloatOption(options, "spectralFloor", cfg.spectral_floor);
    cfg.wpe_enabled = repairBoolOption(options, "wpeEnabled", cfg.wpe_enabled);
    cfg.wpe_iterations = repairIntOption(options, "wpeIterations", cfg.wpe_iterations);
    cfg.wpe_taps = repairIntOption(options, "wpeTaps", cfg.wpe_taps);
    cfg.wpe_strength = repairFloatOption(options, "wpeStrength", cfg.wpe_strength);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: hopLength must be in (0, nFft]");
  }
  Audio result = mastering::repair::dereverb_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_trim_silence(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  mastering::repair::TrimSilenceConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    if (hasProperty(options, "paddingSamples")) {
      const int v =
          repairIntOption(options, "paddingSamples", static_cast<int>(cfg.padding_samples));
      if (v < 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "masteringRepairTrimSilence: paddingSamples must be non-negative");
      }
      cfg.padding_samples = static_cast<size_t>(v);
    }
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseTrimSilenceMode(value.as<std::string>(), cfg.mode);
      }
    }
    cfg.gate_lufs = repairFloatOption(options, "gateLufs", cfg.gate_lufs);
    cfg.window_ms = repairFloatOption(options, "windowMs", cfg.window_ms);
  }
  Audio result = mastering::repair::trim_silence(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

void registerRepairBindings() {
  // Mastering — offline repair processors
  function("masteringRepairDeclick", &js_mastering_repair_declick);
  function("masteringRepairDenoiseClassical", &js_mastering_repair_denoise_classical);
  function("masteringRepairDeclip", &js_mastering_repair_declip);
  function("masteringRepairDecrackle", &js_mastering_repair_decrackle);
  function("masteringRepairDehum", &js_mastering_repair_dehum);
  function("masteringRepairDereverbClassical", &js_mastering_repair_dereverb_classical);
  function("masteringRepairTrimSilence", &js_mastering_repair_trim_silence);
}

#endif  // __EMSCRIPTEN__
