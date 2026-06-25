/// @file offline_dynamics_editing.cpp
/// @brief Embind bindings for offline dynamics, scale quantizer, and resample APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

namespace {

mastering::dynamics::DetectorMode parseCompressorDetector(
    val value, mastering::dynamics::DetectorMode fallback) {
  const std::string type = value.typeOf().as<std::string>();
  if (type == "number") {
    const int code = value.as<int>();
    switch (code) {
      case 0:
        return mastering::dynamics::DetectorMode::Peak;
      case 1:
        return mastering::dynamics::DetectorMode::Rms;
      case 2:
        return mastering::dynamics::DetectorMode::LogRms;
      default:
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringDynamicsCompressor: unknown detector code");
    }
  }
  if (type == "string") {
    std::string s = value.as<std::string>();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "peak") return mastering::dynamics::DetectorMode::Peak;
    if (s == "rms") return mastering::dynamics::DetectorMode::Rms;
    if (s == "log_rms" || s == "logrms") return mastering::dynamics::DetectorMode::LogRms;
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringDynamicsCompressor: unknown detector mode: " + s);
  }
  return fallback;
}

template <typename Processor>
void runDynamicsOffline(Processor& processor, std::vector<float>& samples, int sample_rate,
                        int& latency_samples_out) {
  if (samples.empty()) {
    latency_samples_out = 0;
    return;
  }
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
  latency_samples_out = processor.latency_samples();
}

val makeDynamicsResult(const std::vector<float>& samples, int latency_samples) {
  val out = val::object();
  out.set("samples", vectorToFloat32Array(samples));
  out.set("latencySamples", latency_samples);
  return out;
}

}  // namespace

val js_mastering_dynamics_compressor(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::CompressorConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("thresholdDb")) {
      cfg.threshold_db = options["thresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("ratio")) cfg.ratio = options["ratio"].as<float>();
    if (options.hasOwnProperty("attackMs")) cfg.attack_ms = options["attackMs"].as<float>();
    if (options.hasOwnProperty("releaseMs")) cfg.release_ms = options["releaseMs"].as<float>();
    if (options.hasOwnProperty("kneeDb")) cfg.knee_db = options["kneeDb"].as<float>();
    if (options.hasOwnProperty("makeupGainDb")) {
      cfg.makeup_gain_db = options["makeupGainDb"].as<float>();
    }
    if (options.hasOwnProperty("autoMakeup")) cfg.auto_makeup = options["autoMakeup"].as<bool>();
    if (options.hasOwnProperty("detector")) {
      cfg.detector = parseCompressorDetector(options["detector"], cfg.detector);
    }
    if (options.hasOwnProperty("sidechainHpfEnabled")) {
      cfg.sidechain_hpf_enabled = options["sidechainHpfEnabled"].as<bool>();
    }
    if (options.hasOwnProperty("sidechainHpfHz")) {
      cfg.sidechain_hpf_hz = options["sidechainHpfHz"].as<float>();
    }
    if (options.hasOwnProperty("pdrTimeMs")) cfg.pdr_time_ms = options["pdrTimeMs"].as<float>();
    if (options.hasOwnProperty("pdrReleaseScale")) {
      cfg.pdr_release_scale = options["pdrReleaseScale"].as<float>();
    }
  }
  mastering::dynamics::Compressor processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_gate(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::GateConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("thresholdDb")) {
      cfg.threshold_db = options["thresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("attackMs")) cfg.attack_ms = options["attackMs"].as<float>();
    if (options.hasOwnProperty("releaseMs")) cfg.release_ms = options["releaseMs"].as<float>();
    if (options.hasOwnProperty("rangeDb")) cfg.range_db = options["rangeDb"].as<float>();
    if (options.hasOwnProperty("holdMs")) cfg.hold_ms = options["holdMs"].as<float>();
    if (options.hasOwnProperty("closeThresholdDb")) {
      cfg.close_threshold_db = options["closeThresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("keyHpfHz")) cfg.key_hpf_hz = options["keyHpfHz"].as<float>();
  }
  mastering::dynamics::Gate processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_transient_shaper(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::TransientShaperConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("attackGainDb")) {
      cfg.attack_gain_db = options["attackGainDb"].as<float>();
    }
    if (options.hasOwnProperty("sustainGainDb")) {
      cfg.sustain_gain_db = options["sustainGainDb"].as<float>();
    }
    if (options.hasOwnProperty("fastAttackMs")) {
      cfg.fast_attack_ms = options["fastAttackMs"].as<float>();
    }
    if (options.hasOwnProperty("fastReleaseMs")) {
      cfg.fast_release_ms = options["fastReleaseMs"].as<float>();
    }
    if (options.hasOwnProperty("slowAttackMs")) {
      cfg.slow_attack_ms = options["slowAttackMs"].as<float>();
    }
    if (options.hasOwnProperty("slowReleaseMs")) {
      cfg.slow_release_ms = options["slowReleaseMs"].as<float>();
    }
    if (options.hasOwnProperty("sensitivity")) {
      cfg.sensitivity = options["sensitivity"].as<float>();
    }
    if (options.hasOwnProperty("maxGainDb")) cfg.max_gain_db = options["maxGainDb"].as<float>();
    if (options.hasOwnProperty("gainSmoothingMs")) {
      cfg.gain_smoothing_ms = options["gainSmoothingMs"].as<float>();
    }
    if (options.hasOwnProperty("lookaheadMs")) {
      cfg.lookahead_ms = options["lookaheadMs"].as<float>();
    }
  }
  mastering::dynamics::TransientShaper processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

// ============================================================================
// Editing — 12-TET scale quantizer
// ============================================================================

namespace {

editing::pitch_editor::ScaleQuantizerConfig makeScaleConfig(int root, int mode_mask,
                                                            float reference_midi) {
  if (root < 0 || root > 11) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scaleQuantizer: root must be in [0, 11]");
  }
  if (mode_mask == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scaleQuantizer: modeMask must be non-zero");
  }
  if (mode_mask < 0 || mode_mask > 4095) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scaleQuantizer: modeMask must be in [0, 4095]");
  }
  editing::pitch_editor::ScaleQuantizerConfig cfg;
  cfg.root = root;
  cfg.mode_mask = static_cast<uint16_t>(mode_mask);
  if (reference_midi > 0.0f) cfg.reference_midi = reference_midi;
  return cfg;
}

}  // namespace

float js_scale_quantize_midi(int root, int mode_mask, float midi, float reference_midi) {
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, reference_midi));
  return q.quantize_midi(midi);
}

float js_scale_correction_semitones(int root, int mode_mask, float midi, float reference_midi) {
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, reference_midi));
  return q.correction_semitones(midi);
}

bool js_scale_pitch_class_enabled(int root, int mode_mask, int pitch_class) {
  if (pitch_class < 0 || pitch_class > 11) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scalePitchClassEnabled: pitchClass must be in [0, 11]");
  }
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, 0.0f));
  return q.pitch_class_enabled(pitch_class);
}

// ============================================================================
// Core - Resample
// ============================================================================

val js_resample(val samples, int src_sr, int target_sr) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> result = resample(data.data(), data.size(), src_sr, target_sr);
  return vectorToFloat32Array(result);
}

void registerOfflineDynamicsEditingBindings() {
  // Mastering — offline dynamics processors
  function("masteringDynamicsCompressor", &js_mastering_dynamics_compressor);
  function("masteringDynamicsGate", &js_mastering_dynamics_gate);
  function("masteringDynamicsTransientShaper", &js_mastering_dynamics_transient_shaper);

  // Editing — scale quantizer
  function("scaleQuantizeMidi", &js_scale_quantize_midi);
  function("scaleCorrectionSemitones", &js_scale_correction_semitones);
  function("scalePitchClassEnabled", &js_scale_pitch_class_enabled);

  // Core - Resample
  function("resample", &js_resample);
}

#endif  // __EMSCRIPTEN__
