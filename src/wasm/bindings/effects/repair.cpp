/// @file repair.cpp
/// @brief Embind bindings for offline mastering repair APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>

#include "wasm/bindings/common/common.h"

// ============================================================================
// Mastering — offline repair processors (declick / denoise_classical)
// ============================================================================

namespace {

// Missing, null, undefined, and values of the wrong primitive type all retain
// the config default, so embind's undefined-to-NaN coercion never reaches an
// integer conversion in a DSP config. The Node addon refuses a wrong-typed
// value by name for most repair fields, so the two surfaces answer that case
// differently.
bool repairOptionValue(const val& options, const char* key, val* value) {
  if (!hasProperty(options, key)) return false;
  *value = options[key];
  return !value->isUndefined() && !value->isNull();
}

int repairIntOption(const val& options, const char* key, int fallback) {
  val value = val::undefined();
  if (!repairOptionValue(options, key, &value) || value.typeOf().as<std::string>() != "number") {
    return fallback;
  }
  // Range-check before narrowing, for the reason intProperty does: val::as<int>()
  // saturates, so 2^31 and 4294967295 both arrived as INT_MAX and produced one
  // identical output that every downstream positivity check accepted.
  return checkedIntFromVal(value, key);
}

float repairFloatOption(const val& options, const char* key, float fallback) {
  val value = val::undefined();
  if (!repairOptionValue(options, key, &value) || value.typeOf().as<std::string>() != "number") {
    return fallback;
  }
  // Range-check before narrowing, for the reason repairIntOption does: a value
  // past FLT_MAX becomes +inf, and the config validator refuses NaN but not inf,
  // so 3.5e38, 1e39, 1e300 and Infinity all produced one identical output.
  return checkedFloatFromVal(value, key);
}

bool repairBoolOption(const val& options, const char* key, bool fallback) {
  val value = val::undefined();
  return repairOptionValue(options, key, &value) && value.typeOf().as<std::string>() == "boolean"
             ? value.as<bool>()
             : fallback;
}

// Read a dereverb options bag over `config`, leaving absent keys alone.
mastering::repair::DereverbClassicalConfig readDereverbConfig(
    const val& options, mastering::repair::DereverbClassicalConfig config) {
  config.threshold = repairFloatOption(options, "threshold", config.threshold);
  config.attenuation = repairFloatOption(options, "attenuation", config.attenuation);
  config.n_fft = repairIntOption(options, "nFft", config.n_fft);
  config.hop_length = repairIntOption(options, "hopLength", config.hop_length);
  config.t60_sec = repairFloatOption(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = repairFloatOption(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = repairFloatOption(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = repairFloatOption(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = repairBoolOption(options, "wpeEnabled", config.wpe_enabled);
  config.wpe_iterations = repairIntOption(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = repairIntOption(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = repairFloatOption(options, "wpeStrength", config.wpe_strength);
  return config;
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

// Throws on an unknown name rather than falling back, matching the C ABI's own
// enum mapping, so there is no default to carry.
mastering::repair::DenoiseMode parseDenoiseMode(const std::string& name) {
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

mastering::repair::DenoiseNoiseEstimator parseDenoiseNoiseEstimator(const std::string& name) {
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
        cfg.mode = parseDenoiseMode(value.as<std::string>());
      }
    }
    if (hasProperty(options, "noiseEstimator")) {
      val value = val::undefined();
      if (repairOptionValue(options, "noiseEstimator", &value)) {
        cfg.noise_estimator = parseDenoiseNoiseEstimator(value.as<std::string>());
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

mastering::repair::DecrackleMode parseDecrackleMode(const std::string& name) {
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

mastering::repair::TrimSilenceMode parseTrimSilenceMode(const std::string& name) {
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
        cfg.mode = parseDecrackleMode(value.as<std::string>());
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
    cfg = readDereverbConfig(options, cfg);
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

val js_mastering_repair_dereverb_config_for_room(val estimate, val options) {
  if (estimate.isUndefined() || estimate.isNull()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbConfigForRoom: a room estimate is required");
  }
  // Read AND written: the caller's config is the base, and only the two fields
  // the measurement determines come back changed.
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  std::vector<float> rt60_bands;
  if (hasProperty(estimate, "rt60Bands")) {
    val bands = estimate["rt60Bands"];
    // Absent bands are an empty set, which the measurement treats as "did not
    // converge" rather than as a zero reverberation time.
    if (!bands.isUndefined() && !bands.isNull()) rt60_bands = float32ArrayToVector(bands);
  }
  // apply_room_measurement takes a non-positive or non-finite volume as "no
  // measurement" and leaves lateDelayMs at the caller's value, which is also
  // what the C ABI does with the same field.
  mastering::repair::apply_room_measurement(cfg, mid_frequency_rt60(rt60_bands),
                                            floatOption(estimate, "volume", 0.0f));

  val out = val::object();
  out.set("threshold", cfg.threshold);
  out.set("attenuation", cfg.attenuation);
  out.set("nFft", cfg.n_fft);
  out.set("hopLength", cfg.hop_length);
  out.set("t60Sec", cfg.t60_sec);
  out.set("lateDelayMs", cfg.late_delay_ms);
  out.set("overSubtraction", cfg.over_subtraction);
  out.set("spectralFloor", cfg.spectral_floor);
  out.set("wpeEnabled", cfg.wpe_enabled);
  out.set("wpeIterations", cfg.wpe_iterations);
  out.set("wpeTaps", cfg.wpe_taps);
  out.set("wpeStrength", cfg.wpe_strength);
  return out;
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
        cfg.mode = parseTrimSilenceMode(value.as<std::string>());
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
  function("masteringRepairDereverbConfigForRoom", &js_mastering_repair_dereverb_config_for_room);
  function("masteringRepairTrimSilence", &js_mastering_repair_trim_silence);
}

#endif  // __EMSCRIPTEN__
