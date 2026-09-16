/// @file offline_dynamics_editing.cpp
/// @brief Embind bindings for offline dynamics, scale quantizer, and resample APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>

#include "editing/pitch_editor/scale_quantizer.h"
#include "util/zero_is_default.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

namespace {

mastering::dynamics::DetectorMode parseCompressorDetector(
    val value, mastering::dynamics::DetectorMode fallback) {
  const std::string type = value.typeOf().as<std::string>();
  if (type == "number") {
    // An ordinal names a member, not a quantity: 1.5 used to select Rms and NaN
    // Peak, neither of which the caller spelled.
    const int code = checkedIntFromVal(value, "masteringDynamicsCompressor detector");
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
  // Keep the direct WASM binding aligned with the C-ABI/Python offline path:
  // drain lookahead latency and return an input-aligned buffer, rather than
  // exposing a delayed buffer with a latency value the caller cannot apply.
  mastering::api::internal::run_processor_mono(processor, samples, sample_rate);
  latency_samples_out = processor.latency_samples();
}

val makeDynamicsResult(const std::vector<float>& samples, int latency_samples) {
  val out = val::object();
  out.set("samples", vectorToFloat32Array(samples));
  out.set("latencySamples", latency_samples);
  return out;
}

}  // namespace

val js_mastering_dynamics_compressor(val samples, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::CompressorConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "thresholdDb")) {
      cfg.threshold_db = checkedFloatFromVal(options["thresholdDb"], "thresholdDb");
    }
    if (hasProperty(options, "ratio")) cfg.ratio = checkedFloatFromVal(options["ratio"], "ratio");
    if (hasProperty(options, "attackMs")) {
      cfg.attack_ms = checkedFloatFromVal(options["attackMs"], "attackMs");
    }
    if (hasProperty(options, "releaseMs")) {
      cfg.release_ms = checkedFloatFromVal(options["releaseMs"], "releaseMs");
    }
    if (hasProperty(options, "kneeDb")) {
      cfg.knee_db = checkedFloatFromVal(options["kneeDb"], "kneeDb");
    }
    if (hasProperty(options, "makeupGainDb")) {
      cfg.makeup_gain_db = checkedFloatFromVal(options["makeupGainDb"], "makeupGainDb");
    }
    if (hasProperty(options, "autoMakeup")) cfg.auto_makeup = options["autoMakeup"].as<bool>();
    if (hasProperty(options, "detector")) {
      cfg.detector = parseCompressorDetector(options["detector"], cfg.detector);
    }
    if (hasProperty(options, "sidechainHpfEnabled")) {
      cfg.sidechain_hpf_enabled = options["sidechainHpfEnabled"].as<bool>();
    }
    if (hasProperty(options, "sidechainHpfHz")) {
      cfg.sidechain_hpf_hz = checkedFloatFromVal(options["sidechainHpfHz"], "sidechainHpfHz");
    }
    if (hasProperty(options, "pdrTimeMs")) {
      cfg.pdr_time_ms = checkedFloatFromVal(options["pdrTimeMs"], "pdrTimeMs");
    }
    if (hasProperty(options, "pdrReleaseScale")) {
      cfg.pdr_release_scale = checkedFloatFromVal(options["pdrReleaseScale"], "pdrReleaseScale");
    }
  }
  mastering::dynamics::Compressor processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_gate(val samples, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::GateConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "thresholdDb")) {
      cfg.threshold_db = checkedFloatFromVal(options["thresholdDb"], "thresholdDb");
    }
    if (hasProperty(options, "attackMs")) {
      cfg.attack_ms = checkedFloatFromVal(options["attackMs"], "attackMs");
    }
    if (hasProperty(options, "releaseMs")) {
      cfg.release_ms = checkedFloatFromVal(options["releaseMs"], "releaseMs");
    }
    if (hasProperty(options, "rangeDb")) {
      cfg.range_db = checkedFloatFromVal(options["rangeDb"], "rangeDb");
    }
    if (hasProperty(options, "holdMs")) {
      cfg.hold_ms = checkedFloatFromVal(options["holdMs"], "holdMs");
    }
    if (hasProperty(options, "closeThresholdDb")) {
      cfg.close_threshold_db = checkedFloatFromVal(options["closeThresholdDb"], "closeThresholdDb");
    }
    if (hasProperty(options, "keyHpfHz")) {
      cfg.key_hpf_hz = checkedFloatFromVal(options["keyHpfHz"], "keyHpfHz");
    }
  }
  mastering::dynamics::Gate processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_transient_shaper(val samples, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::dynamics::TransientShaperConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "attackGainDb")) {
      cfg.attack_gain_db = checkedFloatFromVal(options["attackGainDb"], "attackGainDb");
    }
    if (hasProperty(options, "sustainGainDb")) {
      cfg.sustain_gain_db = checkedFloatFromVal(options["sustainGainDb"], "sustainGainDb");
    }
    if (hasProperty(options, "fastAttackMs")) {
      cfg.fast_attack_ms = checkedFloatFromVal(options["fastAttackMs"], "fastAttackMs");
    }
    if (hasProperty(options, "fastReleaseMs")) {
      cfg.fast_release_ms = checkedFloatFromVal(options["fastReleaseMs"], "fastReleaseMs");
    }
    if (hasProperty(options, "slowAttackMs")) {
      cfg.slow_attack_ms = checkedFloatFromVal(options["slowAttackMs"], "slowAttackMs");
    }
    if (hasProperty(options, "slowReleaseMs")) {
      cfg.slow_release_ms = checkedFloatFromVal(options["slowReleaseMs"], "slowReleaseMs");
    }
    if (hasProperty(options, "sensitivity")) {
      cfg.sensitivity = checkedFloatFromVal(options["sensitivity"], "sensitivity");
    }
    if (hasProperty(options, "maxGainDb")) {
      cfg.max_gain_db = checkedFloatFromVal(options["maxGainDb"], "maxGainDb");
    }
    if (hasProperty(options, "gainSmoothingMs")) {
      cfg.gain_smoothing_ms = checkedFloatFromVal(options["gainSmoothingMs"], "gainSmoothingMs");
    }
    if (hasProperty(options, "lookaheadMs")) {
      cfg.lookahead_ms = checkedFloatFromVal(options["lookaheadMs"], "lookaheadMs");
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
  cfg.reference_midi = ZeroIsDefault(reference_midi)
                           .checked(cfg.reference_midi, 0.0f,
                                    editing::pitch_editor::kMaxReferenceMidi, "reference_midi");
  return cfg;
}

}  // namespace

float js_scale_quantize_midi(const val& root, const val& mode_mask, const val& midi,
                             const val& reference_midi_val) {
  const float reference_midi = checkedFloatFromVal(reference_midi_val, "referenceMidi");
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(
      checkedIntFromVal(root, "root"), checkedIntFromVal(mode_mask, "modeMask"), reference_midi));
  return q.quantize_midi(checkedFloatFromVal(midi, "midi"));
}

float js_scale_correction_semitones(const val& root, const val& mode_mask, const val& midi,
                                    const val& reference_midi_val) {
  const float reference_midi = checkedFloatFromVal(reference_midi_val, "referenceMidi");
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(
      checkedIntFromVal(root, "root"), checkedIntFromVal(mode_mask, "modeMask"), reference_midi));
  return q.correction_semitones(checkedFloatFromVal(midi, "midi"));
}

bool js_scale_pitch_class_enabled(const val& root, const val& mode_mask,
                                  const val& pitch_class_val) {
  const int pitch_class = checkedIntFromVal(pitch_class_val, "pitchClass");
  if (pitch_class < 0 || pitch_class > 11) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scalePitchClassEnabled: pitchClass must be in [0, 11]");
  }
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(
      checkedIntFromVal(root, "root"), checkedIntFromVal(mode_mask, "modeMask"), 0.0f));
  return q.pitch_class_enabled(pitch_class);
}

// ============================================================================
// Core - Resample
// ============================================================================

val js_resample(val samples, const val& src_sr_val, const val& target_sr_val) {
  const int src_sr = checkedIntFromVal(src_sr_val, "srcSr");
  const int target_sr = checkedIntFromVal(target_sr_val, "targetSr");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), src_sr);
  if (target_sr < kMinAudioSampleRate || target_sr > kMaxAudioSampleRate) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "resample: target sample rate is out of range");
  }
  const double projected = static_cast<double>(data.size()) * static_cast<double>(target_sr) /
                           static_cast<double>(src_sr);
  if (projected > static_cast<double>(kMaxAudioBufferSize)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "resample: output buffer would be too large");
  }
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
