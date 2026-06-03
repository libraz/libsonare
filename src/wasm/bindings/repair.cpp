/// @file repair.cpp
/// @brief Embind bindings for offline mastering repair APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Mastering — offline repair processors (declick / denoise_classical)
// ============================================================================

val js_mastering_repair_declick(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) {
      cfg.threshold = options["threshold"].as<float>();
    }
    if (options.hasOwnProperty("neighborRatio")) {
      cfg.neighbor_ratio = options["neighborRatio"].as<float>();
    }
    if (options.hasOwnProperty("maxClickSamples")) {
      const int v = options["maxClickSamples"].as<int>();
      if (v <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringRepairDeclick: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    if (options.hasOwnProperty("lpcOrder")) {
      cfg.lpc_order = options["lpcOrder"].as<int>();
    }
    if (options.hasOwnProperty("residualRatio")) {
      cfg.residual_ratio = options["residualRatio"].as<float>();
    }
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
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseDenoiseMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("noiseEstimator")) {
      cfg.noise_estimator = parseDenoiseNoiseEstimator(options["noiseEstimator"].as<std::string>(),
                                                       cfg.noise_estimator);
    }
    if (options.hasOwnProperty("nFft")) cfg.n_fft = options["nFft"].as<int>();
    if (options.hasOwnProperty("hopLength")) cfg.hop_length = options["hopLength"].as<int>();
    if (options.hasOwnProperty("ddAlpha")) cfg.dd_alpha = options["ddAlpha"].as<float>();
    if (options.hasOwnProperty("gainFloor")) cfg.gain_floor = options["gainFloor"].as<float>();
    if (options.hasOwnProperty("overSubtraction")) {
      cfg.over_subtraction = options["overSubtraction"].as<float>();
    }
    if (options.hasOwnProperty("spectralFloor")) {
      cfg.spectral_floor = options["spectralFloor"].as<float>();
    }
    if (options.hasOwnProperty("noiseEstimationQuantile")) {
      cfg.noise_estimation_quantile = options["noiseEstimationQuantile"].as<float>();
    }
    if (options.hasOwnProperty("speechPresenceGain")) {
      cfg.speech_presence_gain = options["speechPresenceGain"].as<bool>();
    }
    if (options.hasOwnProperty("gainSmoothing")) {
      cfg.gain_smoothing = options["gainSmoothing"].as<bool>();
    }
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
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("clipThreshold")) {
      cfg.clip_threshold = options["clipThreshold"].as<float>();
    }
    if (options.hasOwnProperty("lpcOrder")) cfg.lpc_order = options["lpcOrder"].as<int>();
    if (options.hasOwnProperty("iterations")) cfg.iterations = options["iterations"].as<int>();
    if (options.hasOwnProperty("lpcBlend")) cfg.lpc_blend = options["lpcBlend"].as<float>();
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
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseDecrackleMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("levels")) cfg.levels = options["levels"].as<int>();
  }
  Audio result = mastering::repair::decrackle(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dehum(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("fundamentalHz")) {
      cfg.fundamental_hz = options["fundamentalHz"].as<float>();
    }
    if (options.hasOwnProperty("harmonics")) cfg.harmonics = options["harmonics"].as<int>();
    if (options.hasOwnProperty("q")) cfg.q = options["q"].as<float>();
    if (options.hasOwnProperty("adaptive")) cfg.adaptive = options["adaptive"].as<bool>();
    if (options.hasOwnProperty("searchRangeHz")) {
      cfg.search_range_hz = options["searchRangeHz"].as<float>();
    }
    if (options.hasOwnProperty("adaptation")) cfg.adaptation = options["adaptation"].as<float>();
    if (options.hasOwnProperty("frameSize")) cfg.frame_size = options["frameSize"].as<int>();
    if (options.hasOwnProperty("pllBandwidth")) {
      cfg.pll_bandwidth = options["pllBandwidth"].as<float>();
    }
  }
  Audio result = mastering::repair::dehum(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dereverb_classical(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("attenuation")) {
      cfg.attenuation = options["attenuation"].as<float>();
    }
    if (options.hasOwnProperty("nFft")) cfg.n_fft = options["nFft"].as<int>();
    if (options.hasOwnProperty("hopLength")) cfg.hop_length = options["hopLength"].as<int>();
    if (options.hasOwnProperty("t60Sec")) cfg.t60_sec = options["t60Sec"].as<float>();
    if (options.hasOwnProperty("lateDelayMs")) {
      cfg.late_delay_ms = options["lateDelayMs"].as<float>();
    }
    if (options.hasOwnProperty("overSubtraction")) {
      cfg.over_subtraction = options["overSubtraction"].as<float>();
    }
    if (options.hasOwnProperty("spectralFloor")) {
      cfg.spectral_floor = options["spectralFloor"].as<float>();
    }
    if (options.hasOwnProperty("wpeEnabled")) {
      cfg.wpe_enabled = options["wpeEnabled"].as<bool>();
    }
    if (options.hasOwnProperty("wpeIterations")) {
      cfg.wpe_iterations = options["wpeIterations"].as<int>();
    }
    if (options.hasOwnProperty("wpeTaps")) cfg.wpe_taps = options["wpeTaps"].as<int>();
    if (options.hasOwnProperty("wpeStrength")) {
      cfg.wpe_strength = options["wpeStrength"].as<float>();
    }
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
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::TrimSilenceConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("paddingSamples")) {
      const int v = options["paddingSamples"].as<int>();
      if (v < 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "masteringRepairTrimSilence: paddingSamples must be non-negative");
      }
      cfg.padding_samples = static_cast<size_t>(v);
    }
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseTrimSilenceMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("gateLufs")) cfg.gate_lufs = options["gateLufs"].as<float>();
    if (options.hasOwnProperty("windowMs")) cfg.window_ms = options["windowMs"].as<float>();
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
