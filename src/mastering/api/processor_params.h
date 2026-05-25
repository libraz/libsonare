#pragma once

/// @file processor_params.h
/// @brief Internal helpers shared by named_processor.cpp and insert_factory.cpp
///        for turning a flat list of (key, value) params into processor configs.
///
/// This header is INTERNAL to the mastering API; it is not part of the public
/// surface. It centralizes the param-name -> config-field mapping so the
/// offline (named_processor) and streaming (insert_factory) paths stay in sync.

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mastering/api/named_processor.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"
#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/dither.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/crossover.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/haas_enhancer.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::api::detail {

using ParamMap = std::unordered_map<std::string, double>;

inline ParamMap make_map(const std::vector<Param>& params) {
  ParamMap map;
  for (const auto& param : params) {
    map[param.key] = param.value;
  }
  return map;
}

inline float f(const ParamMap& params, const char* key, float default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : static_cast<float>(it->second);
}

inline int i(const ParamMap& params, const char* key, int default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : static_cast<int>(std::lround(it->second));
}

inline bool b(const ParamMap& params, const char* key, bool default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : it->second != 0.0;
}

inline std::vector<float> cutoffs(const ParamMap& params) {
  std::vector<float> values;
  for (int index = 0; index < 8; ++index) {
    const std::string key = "cutoff" + std::to_string(index) + "Hz";
    auto it = params.find(key);
    if (it != params.end()) {
      values.push_back(static_cast<float>(it->second));
    }
  }
  return values;
}

inline eq::EqBandType eq_band_type(int value) {
  switch (value) {
    case 1:
      return eq::EqBandType::LowShelf;
    case 2:
      return eq::EqBandType::HighShelf;
    case 3:
      return eq::EqBandType::LowPass;
    case 4:
      return eq::EqBandType::HighPass;
    case 5:
      return eq::EqBandType::BandPass;
    case 6:
      return eq::EqBandType::Notch;
    case 7:
      return eq::EqBandType::TiltShelf;
    case 8:
      return eq::EqBandType::FlatTilt;
    default:
      return eq::EqBandType::Peak;
  }
}

inline eq::StereoPlacement stereo_placement(int value) {
  switch (value) {
    case 1:
      return eq::StereoPlacement::Left;
    case 2:
      return eq::StereoPlacement::Right;
    case 3:
      return eq::StereoPlacement::Mid;
    case 4:
      return eq::StereoPlacement::Side;
    default:
      return eq::StereoPlacement::Stereo;
  }
}

inline eq::PhaseMode phase_mode(int value) {
  switch (value) {
    case 1:
      return eq::PhaseMode::ZeroLatency;
    case 2:
      return eq::PhaseMode::NaturalPhase;
    case 3:
      return eq::PhaseMode::LinearPhase;
    default:
      return eq::PhaseMode::Inherit;
  }
}

inline eq::BiquadCoeffMode coeff_mode(int value) {
  switch (value) {
    case 1:
      return eq::BiquadCoeffMode::Vicanek;
    default:
      return eq::BiquadCoeffMode::Rbj;
  }
}

inline bool has_eq_band_params(const ParamMap& params, const std::string& prefix) {
  static constexpr const char* kFields[] = {
      "type",
      "frequencyHz",
      "gainDb",
      "q",
      "enabled",
      "coeffMode",
      "slopeDbOct",
      "placement",
      "phase",
      "soloed",
      "bypassed",
      "proportionalQ",
      "proportionalQStrength",
      "dynamic",
      "thresholdDb",
      "autoThreshold",
      "ratio",
      "rangeDb",
      "attackMs",
      "releaseMs",
      "lookaheadMs",
      "sidechainFreqHz",
      "sidechainQ",
  };
  for (const char* field : kFields) {
    if (params.find(prefix + field) != params.end()) return true;
  }
  return false;
}

inline eq::EqBand eq_band(const ParamMap& params, const std::string& prefix) {
  eq::EqBand band;
  band.type = eq_band_type(i(params, (prefix + "type").c_str(), 0));
  band.coeff_mode = coeff_mode(i(params, (prefix + "coeffMode").c_str(), 0));
  band.frequency_hz = f(params, (prefix + "frequencyHz").c_str(), band.frequency_hz);
  band.gain_db = f(params, (prefix + "gainDb").c_str(), band.gain_db);
  band.q = f(params, (prefix + "q").c_str(), band.q);
  band.enabled = b(params, (prefix + "enabled").c_str(), true);
  band.slope_db_oct = i(params, (prefix + "slopeDbOct").c_str(), band.slope_db_oct);
  band.placement = stereo_placement(i(params, (prefix + "placement").c_str(), 0));
  band.phase = phase_mode(i(params, (prefix + "phase").c_str(), 0));
  band.soloed = b(params, (prefix + "soloed").c_str(), band.soloed);
  band.bypassed = b(params, (prefix + "bypassed").c_str(), band.bypassed);
  band.proportional_q = b(params, (prefix + "proportionalQ").c_str(), band.proportional_q);
  band.proportional_q_strength =
      f(params, (prefix + "proportionalQStrength").c_str(), band.proportional_q_strength);
  band.dyn.enabled = b(params, (prefix + "dynamic").c_str(), band.dyn.enabled);
  band.dyn.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.dyn.threshold_db);
  band.dyn.auto_threshold = b(params, (prefix + "autoThreshold").c_str(), band.dyn.auto_threshold);
  band.dyn.ratio = f(params, (prefix + "ratio").c_str(), band.dyn.ratio);
  band.dyn.range_db = f(params, (prefix + "rangeDb").c_str(), band.dyn.range_db);
  band.dyn.attack_ms = f(params, (prefix + "attackMs").c_str(), band.dyn.attack_ms);
  band.dyn.release_ms = f(params, (prefix + "releaseMs").c_str(), band.dyn.release_ms);
  band.dyn.lookahead_ms = f(params, (prefix + "lookaheadMs").c_str(), band.dyn.lookahead_ms);
  band.dyn.sidechain_freq_hz =
      f(params, (prefix + "sidechainFreqHz").c_str(), band.dyn.sidechain_freq_hz);
  band.dyn.sidechain_q = f(params, (prefix + "sidechainQ").c_str(), band.dyn.sidechain_q);
  return band;
}

inline void configure_parametric(eq::ParametricEq& processor, const ParamMap& params,
                                 const std::string& prefix = "band") {
  for (size_t index = 0; index < eq::ParametricEq::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    if (has_eq_band_params(params, band_prefix)) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

inline void configure_equalizer(eq::EqualizerProcessor& processor, const ParamMap& params,
                                const std::string& prefix = "band") {
  processor.set_auto_gain_enabled(b(params, "autoGain", processor.auto_gain_enabled()));
  processor.set_phase_mode(
      phase_mode(i(params, "phaseMode", static_cast<int>(processor.phase_mode()))));
  for (size_t index = 0; index < eq::EqualizerProcessor::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    if (has_eq_band_params(params, band_prefix)) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

inline dynamics::CompressorConfig compressor_config(const ParamMap& params) {
  dynamics::CompressorConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.knee_db = f(params, "kneeDb", config.knee_db);
  config.makeup_gain_db = f(params, "makeupGainDb", config.makeup_gain_db);
  config.auto_makeup = b(params, "autoMakeup", config.auto_makeup);
  config.detector = static_cast<dynamics::DetectorMode>(i(params, "detector", 1));
  config.sidechain_hpf_enabled = b(params, "sidechainHpfEnabled", config.sidechain_hpf_enabled);
  config.sidechain_hpf_hz = f(params, "sidechainHpfHz", config.sidechain_hpf_hz);
  config.pdr_time_ms = f(params, "pdrTimeMs", config.pdr_time_ms);
  config.pdr_release_scale = f(params, "pdrReleaseScale", config.pdr_release_scale);
  return config;
}

inline dynamics::LimiterConfig limiter_config(const ParamMap& params) {
  dynamics::LimiterConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  return config;
}

inline multiband::CrossoverConfig crossover_config(const ParamMap& params) {
  multiband::CrossoverConfig config;
  auto values = cutoffs(params);
  if (!values.empty()) {
    config.cutoffs_hz = values;
  }
  config.slope = static_cast<multiband::CrossoverSlope>(i(params, "slope", 1));
  config.mode = static_cast<multiband::CrossoverMode>(i(params, "mode", 0));
  config.fir_kernel_size = i(params, "firKernelSize", config.fir_kernel_size);
  return config;
}

// ---------------------------------------------------------------------------
// Dynamics
// ---------------------------------------------------------------------------

inline dynamics::BrickwallLimiterConfig brickwall_limiter_config(const ParamMap& params) {
  dynamics::BrickwallLimiterConfig config;
  config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  return config;
}

inline dynamics::DeEsserConfig deesser_config(const ParamMap& params) {
  dynamics::DeEsserConfig config;
  config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  return config;
}

inline dynamics::ExpanderConfig expander_config(const ParamMap& params) {
  dynamics::ExpanderConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  return config;
}

inline dynamics::GateConfig gate_config(const ParamMap& params) {
  dynamics::GateConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  config.hold_ms = f(params, "holdMs", config.hold_ms);
  config.close_threshold_db = f(params, "closeThresholdDb", config.close_threshold_db);
  config.key_hpf_hz = f(params, "keyHpfHz", config.key_hpf_hz);
  return config;
}

inline dynamics::ParallelCompConfig parallel_comp_config(const ParamMap& params) {
  dynamics::ParallelCompConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.makeup_gain_db = f(params, "makeupGainDb", config.makeup_gain_db);
  config.mix = f(params, "mix", config.mix);
  config.linked_detection = b(params, "linkedDetection", config.linked_detection);
  config.output_limiter = b(params, "outputLimiter", config.output_limiter);
  config.output_ceiling_db = f(params, "outputCeilingDb", config.output_ceiling_db);
  return config;
}

inline dynamics::SidechainRouterConfig sidechain_router_config(const ParamMap& params) {
  dynamics::SidechainRouterConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  config.sidechain_hpf_enabled = b(params, "sidechainHpfEnabled", config.sidechain_hpf_enabled);
  config.sidechain_hpf_hz = f(params, "sidechainHpfHz", config.sidechain_hpf_hz);
  config.mono_summing = b(params, "monoSumming", config.mono_summing);
  config.key_listen = b(params, "keyListen", config.key_listen);
  return config;
}

inline dynamics::TransientShaperConfig transient_shaper_config(const ParamMap& params) {
  dynamics::TransientShaperConfig config;
  config.attack_gain_db = f(params, "attackGainDb", config.attack_gain_db);
  config.sustain_gain_db = f(params, "sustainGainDb", config.sustain_gain_db);
  config.fast_attack_ms = f(params, "fastAttackMs", config.fast_attack_ms);
  config.fast_release_ms = f(params, "fastReleaseMs", config.fast_release_ms);
  config.slow_attack_ms = f(params, "slowAttackMs", config.slow_attack_ms);
  config.slow_release_ms = f(params, "slowReleaseMs", config.slow_release_ms);
  config.sensitivity = f(params, "sensitivity", config.sensitivity);
  config.max_gain_db = f(params, "maxGainDb", config.max_gain_db);
  config.gain_smoothing_ms = f(params, "gainSmoothingMs", config.gain_smoothing_ms);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  return config;
}

inline dynamics::UpwardCompressorConfig upward_compressor_config(const ParamMap& params) {
  dynamics::UpwardCompressorConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  return config;
}

inline dynamics::UpwardExpanderConfig upward_expander_config(const ParamMap& params) {
  dynamics::UpwardExpanderConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.ratio = f(params, "ratio", config.ratio);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  return config;
}

inline dynamics::VocalRiderConfig vocal_rider_config(const ParamMap& params) {
  dynamics::VocalRiderConfig config;
  config.target_db = f(params, "targetDb", config.target_db);
  config.max_boost_db = f(params, "maxBoostDb", config.max_boost_db);
  config.max_cut_db = f(params, "maxCutDb", config.max_cut_db);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
  config.gain_smoothing_ms = f(params, "gainSmoothingMs", config.gain_smoothing_ms);
  config.noise_floor_db = f(params, "noiseFloorDb", config.noise_floor_db);
  config.linked_detection = b(params, "linkedDetection", config.linked_detection);
  return config;
}

// ---------------------------------------------------------------------------
// EQ (setter-based and config-based)
// ---------------------------------------------------------------------------

inline void configure_tilt(eq::TiltEq& p, const ParamMap& params) {
  p.set_tilt_db(f(params, "tiltDb", 0.0f));
  p.set_pivot_hz(f(params, "pivotHz", 1000.0f));
}

inline void configure_api_style(eq::ApiStyleEq& p, const ParamMap& params) {
  p.set_band(eq::ApiStyleEq::Band::Low, f(params, "lowFrequencyHz", 100.0f),
             f(params, "lowGainDb", 0.0f));
  p.set_band(eq::ApiStyleEq::Band::LowMid, f(params, "lowMidFrequencyHz", 400.0f),
             f(params, "lowMidGainDb", 0.0f));
  p.set_band(eq::ApiStyleEq::Band::HighMid, f(params, "highMidFrequencyHz", 3000.0f),
             f(params, "highMidGainDb", 0.0f));
  p.set_band(eq::ApiStyleEq::Band::High, f(params, "highFrequencyHz", 10000.0f),
             f(params, "highGainDb", 0.0f));
}

inline void configure_minimum_phase(eq::MinimumPhaseEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::MinimumPhaseEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    if (params.find(prefix + "frequencyHz") != params.end() ||
        params.find(prefix + "gainDb") != params.end()) {
      p.set_band(index, eq_band(params, prefix));
    }
  }
}

inline eq::LinearPhaseEqConfig linear_phase_config(const ParamMap& params) {
  eq::LinearPhaseEqConfig config;
  config.resolution = static_cast<eq::LinearPhaseEqConfig::Resolution>(i(params, "resolution", 0));
  config.fft_size = i(params, "fftSize", config.fft_size);
  config.kernel_size = i(params, "kernelSize", config.kernel_size);
  config.use_partitioned_convolution =
      b(params, "usePartitionedConvolution", config.use_partitioned_convolution);
  config.partition_size = i(params, "partitionSize", config.partition_size);
  return config;
}

inline eq::EqualizerProcessorConfig equalizer_config(const ParamMap& params, int max_channels) {
  eq::EqualizerProcessorConfig config;
  config.max_channels = max_channels;
  config.linear_phase_config = linear_phase_config(params);
  return config;
}

inline void configure_linear_phase_bands(eq::LinearPhaseEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::LinearPhaseEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    if (params.find(prefix + "frequencyHz") != params.end() ||
        params.find(prefix + "gainDb") != params.end()) {
      p.set_band(index, eq_band(params, prefix));
    }
  }
}

inline void configure_dynamic_eq_bands(eq::DynamicEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::DynamicEq::kMaxBands; ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    if (params.find(prefix + "frequencyHz") != params.end()) {
      eq::DynamicEqBand band;
      band.type = eq_band_type(i(params, (prefix + "type").c_str(), 0));
      band.frequency_hz = f(params, (prefix + "frequencyHz").c_str(), band.frequency_hz);
      band.static_gain_db = f(params, (prefix + "staticGainDb").c_str(), band.static_gain_db);
      band.q = f(params, (prefix + "q").c_str(), band.q);
      band.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.threshold_db);
      band.ratio = f(params, (prefix + "ratio").c_str(), band.ratio);
      band.range_db = f(params, (prefix + "rangeDb").c_str(), band.range_db);
      band.enabled = b(params, (prefix + "enabled").c_str(), true);
      band.sidechain_q = f(params, (prefix + "sidechainQ").c_str(), band.sidechain_q);
      band.sidechain_freq_hz =
          f(params, (prefix + "sidechainFreqHz").c_str(), band.sidechain_freq_hz);
      band.attack_ms = f(params, (prefix + "attackMs").c_str(), band.attack_ms);
      band.release_ms = f(params, (prefix + "releaseMs").c_str(), band.release_ms);
      band.lookahead_ms = f(params, (prefix + "lookaheadMs").c_str(), band.lookahead_ms);
      p.set_band(index, band);
    }
  }
}

inline void configure_pultec(eq::PultecEq& p, const ParamMap& params) {
  p.set_low_frequency(f(params, "lowFrequencyHz", 60.0f));
  p.set_low_boost(f(params, "lowBoost", 0.0f));
  p.set_low_attenuation(f(params, "lowAttenuation", 0.0f));
  p.set_high_boost(f(params, "highBoostFrequencyHz", 8000.0f), f(params, "highBoost", 0.0f),
                   f(params, "highBandwidth", 0.5f));
  p.set_high_attenuation(f(params, "highAttenuationFrequencyHz", 10000.0f),
                         f(params, "highAttenuation", 0.0f));
  p.set_component_model(static_cast<eq::PultecComponentModel>(i(params, "componentModel", 0)));
  p.set_output_drive(f(params, "outputDrive", 0.0f));
}

inline void configure_cut_filter(eq::CutFilter& p, const ParamMap& params) {
  p.set_high_pass(f(params, "highPassFrequencyHz", 20.0f),
                  f(params, "highPassQ", constants::kButterworthQ),
                  static_cast<eq::CutFilterSlope>(i(params, "highPassSlope", 0)),
                  b(params, "highPassEnabled", false));
  p.set_low_pass(f(params, "lowPassFrequencyHz", 20000.0f),
                 f(params, "lowPassQ", constants::kButterworthQ),
                 static_cast<eq::CutFilterSlope>(i(params, "lowPassSlope", 0)),
                 b(params, "lowPassEnabled", false));
}

inline void configure_band_pass(eq::BandPassEq& p, const ParamMap& params) {
  p.set_band_pass(f(params, "bandPassFrequencyHz", 1000.0f), f(params, "bandPassQ", 1.0f),
                  b(params, "bandPassEnabled", true));
  p.set_notch(f(params, "notchFrequencyHz", 1000.0f), f(params, "notchQ", 1.0f),
              b(params, "notchEnabled", false));
}

inline void configure_shelving(eq::ShelvingEq& p, const ParamMap& params) {
  p.set_low_shelf(f(params, "lowFrequencyHz", 100.0f), f(params, "lowGainDb", 0.0f),
                  f(params, "lowQ", constants::kButterworthQ), b(params, "lowEnabled", true));
  p.set_high_shelf(f(params, "highFrequencyHz", 10000.0f), f(params, "highGainDb", 0.0f),
                   f(params, "highQ", constants::kButterworthQ), b(params, "highEnabled", true));
}

inline void configure_graphic(eq::GraphicEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::GraphicEq::kNumBands; ++index) {
    const std::string key = "band" + std::to_string(index) + "GainDb";
    if (params.find(key) != params.end()) p.set_gain_db(index, f(params, key.c_str(), 0.0f));
  }
}

inline void configure_mid_side(eq::MidSideEq& p, const ParamMap& params) {
  for (size_t index = 0; index < eq::MidSideEq::kMaxBands; ++index) {
    const std::string mid = "midBand" + std::to_string(index) + ".";
    const std::string side = "sideBand" + std::to_string(index) + ".";
    if (params.find(mid + "frequencyHz") != params.end() ||
        params.find(mid + "gainDb") != params.end()) {
      p.set_mid_band(index, eq_band(params, mid));
    }
    if (params.find(side + "frequencyHz") != params.end() ||
        params.find(side + "gainDb") != params.end()) {
      p.set_side_band(index, eq_band(params, side));
    }
  }
}

// ---------------------------------------------------------------------------
// Saturation
// ---------------------------------------------------------------------------

inline saturation::TapeConfig tape_config(const ParamMap& params) {
  saturation::TapeConfig config;
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.saturation = f(params, "saturation", config.saturation);
  config.hysteresis = f(params, "hysteresis", config.hysteresis);
  config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
  config.speed_ips = f(params, "speedIps", config.speed_ips);
  config.head_bump_db = f(params, "headBumpDb", config.head_bump_db);
  config.bias = f(params, "bias", config.bias);
  config.gap_loss = f(params, "gapLoss", config.gap_loss);
  return config;
}

inline saturation::ExciterConfig exciter_config(const ParamMap& params) {
  saturation::ExciterConfig config;
  config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.amount = f(params, "amount", config.amount);
  config.q = f(params, "q", config.q);
  config.even_odd_mix = f(params, "evenOddMix", config.even_odd_mix);
  return config;
}

inline saturation::BitCrusherConfig bitcrusher_config(const ParamMap& params) {
  saturation::BitCrusherConfig config;
  config.bit_depth = i(params, "bitDepth", config.bit_depth);
  config.downsample_factor = i(params, "downsampleFactor", config.downsample_factor);
  config.mix = f(params, "mix", config.mix);
  config.dither_type = static_cast<final::DitherType>(i(params, "ditherType", 0));
  config.dither_seed = static_cast<uint32_t>(i(params, "ditherSeed", config.dither_seed));
  return config;
}

inline saturation::HardClipperConfig hard_clipper_config(const ParamMap& params) {
  saturation::HardClipperConfig config;
  config.ceiling = f(params, "ceiling", config.ceiling);
  return config;
}

inline saturation::SoftClipperConfig soft_clipper_config(const ParamMap& params) {
  saturation::SoftClipperConfig config;
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.ceiling = f(params, "ceiling", config.ceiling);
  config.mix = f(params, "mix", config.mix);
  return config;
}

inline saturation::WaveshaperConfig waveshaper_config(const ParamMap& params) {
  saturation::WaveshaperConfig config;
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.mix = f(params, "mix", config.mix);
  config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
  config.bias = f(params, "bias", config.bias);
  config.curve = static_cast<saturation::WaveshaperCurve>(i(params, "curve", 0));
  return config;
}

inline saturation::TubeConfig tube_config(const ParamMap& params) {
  saturation::TubeConfig config;
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.bias = f(params, "bias", config.bias);
  config.mix = f(params, "mix", config.mix);
  config.oversample_factor = i(params, "oversampleFactor", config.oversample_factor);
  config.bias_v = f(params, "biasV", config.bias_v);
  config.harmonic_drive = f(params, "harmonicDrive", config.harmonic_drive);
  return config;
}

inline saturation::TransformerConfig transformer_config(const ParamMap& params) {
  saturation::TransformerConfig config;
  config.drive_db = f(params, "driveDb", config.drive_db);
  config.asymmetry = f(params, "asymmetry", config.asymmetry);
  config.mix = f(params, "mix", config.mix);
  return config;
}

inline saturation::MultibandExciterConfig multiband_exciter_config(const ParamMap& params) {
  saturation::MultibandExciterConfig config;
  config.crossover = crossover_config(params);
  for (size_t index = 0; index < config.bands.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    config.bands[index].frequency_hz =
        f(params, (prefix + "frequencyHz").c_str(), config.bands[index].frequency_hz);
    config.bands[index].drive_db =
        f(params, (prefix + "driveDb").c_str(), config.bands[index].drive_db);
    config.bands[index].amount = f(params, (prefix + "amount").c_str(), config.bands[index].amount);
    config.bands[index].q = f(params, (prefix + "q").c_str(), config.bands[index].q);
    config.bands[index].even_odd_mix =
        f(params, (prefix + "evenOddMix").c_str(), config.bands[index].even_odd_mix);
  }
  return config;
}

// ---------------------------------------------------------------------------
// Spectral
// ---------------------------------------------------------------------------

inline spectral::AirBandConfig air_band_config(const ParamMap& params) {
  spectral::AirBandConfig config;
  config.amount = f(params, "amount", config.amount);
  config.shelf_frequency_hz = f(params, "shelfFrequencyHz", config.shelf_frequency_hz);
  config.dynamic_threshold_db = f(params, "dynamicThresholdDb", config.dynamic_threshold_db);
  config.dynamic_range_db = f(params, "dynamicRangeDb", config.dynamic_range_db);
  return config;
}

inline spectral::LowEndFocusConfig low_end_focus_config(const ParamMap& params) {
  spectral::LowEndFocusConfig config;
  config.cutoff_hz = f(params, "cutoffHz", config.cutoff_hz);
  config.width = f(params, "width", config.width);
  config.subharmonic_amount = f(params, "subharmonicAmount", config.subharmonic_amount);
  config.transient_tightness = f(params, "transientTightness", config.transient_tightness);
  return config;
}

inline spectral::PresenceEnhancerConfig presence_enhancer_config(const ParamMap& params) {
  spectral::PresenceEnhancerConfig config;
  config.amount = f(params, "amount", config.amount);
  config.drive = f(params, "drive", config.drive);
  config.center_frequency_hz = f(params, "centerFrequencyHz", config.center_frequency_hz);
  config.q = f(params, "q", config.q);
  return config;
}

inline spectral::SpectralShaperConfig spectral_shaper_config(const ParamMap& params) {
  spectral::SpectralShaperConfig config;
  config.threshold = f(params, "threshold", config.threshold);
  config.amount = f(params, "amount", config.amount);
  config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
  config.high_frequency_hz = f(params, "highFrequencyHz", config.high_frequency_hz);
  config.attack_ms = f(params, "attackMs", config.attack_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.range_db = f(params, "rangeDb", config.range_db);
  return config;
}

// ---------------------------------------------------------------------------
// Stereo
// ---------------------------------------------------------------------------

inline stereo::AutoPanConfig auto_pan_config(const ParamMap& params) {
  stereo::AutoPanConfig config;
  config.rate_hz = f(params, "rateHz", config.rate_hz);
  config.depth = f(params, "depth", config.depth);
  config.phase = f(params, "phase", config.phase);
  return config;
}

inline stereo::HaasEnhancerConfig haas_enhancer_config(const ParamMap& params) {
  stereo::HaasEnhancerConfig config;
  config.delay_ms = f(params, "delayMs", config.delay_ms);
  config.mix = f(params, "mix", config.mix);
  config.delay_right = b(params, "delayRight", config.delay_right);
  return config;
}

inline stereo::ImagerConfig imager_config(const ParamMap& params) {
  stereo::ImagerConfig config;
  config.width = f(params, "width", config.width);
  config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
  config.decorrelation_amount = f(params, "decorrelationAmount", config.decorrelation_amount);
  config.preserve_energy = b(params, "preserveEnergy", config.preserve_energy);
  SONARE_CHECK_RANGE("stereo.imager.width", config.width, 0.0f, 2.0f);
  SONARE_CHECK_RANGE("stereo.imager.decorrelationAmount", config.decorrelation_amount, 0.0f, 1.0f);
  return config;
}

inline stereo::MonoMakerConfig mono_maker_config(const ParamMap& params) {
  stereo::MonoMakerConfig config;
  config.amount = f(params, "amount", config.amount);
  return config;
}

inline stereo::PhaseAlignConfig phase_align_config(const ParamMap& params) {
  stereo::PhaseAlignConfig config;
  config.delay_samples = i(params, "delaySamples", config.delay_samples);
  config.delay_right = b(params, "delayRight", config.delay_right);
  config.fractional_delay_samples =
      f(params, "fractionalDelaySamples", config.fractional_delay_samples);
  return config;
}

inline stereo::StereoBalanceConfig stereo_balance_config(const ParamMap& params) {
  stereo::StereoBalanceConfig config;
  config.balance = f(params, "balance", config.balance);
  config.constant_power = b(params, "constantPower", config.constant_power);
  return config;
}

// ---------------------------------------------------------------------------
// Maximizer
// ---------------------------------------------------------------------------

inline maximizer::MaximizerConfig maximizer_config(const ParamMap& params) {
  maximizer::MaximizerConfig config;
  config.input_gain_db = f(params, "inputGainDb", config.input_gain_db);
  config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  return config;
}

inline maximizer::TruePeakLimiterConfig true_peak_limiter_config(const ParamMap& params) {
  maximizer::TruePeakLimiterConfig config;
  config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  config.oversample_factor = i(params, "oversampleFactor", config.oversample_factor);
  config.apply_gain_at_input_rate =
      b(params, "applyGainAtInputRate", config.apply_gain_at_input_rate);
  return config;
}

inline maximizer::SoftKneeMaxConfig soft_knee_max_config(const ParamMap& params) {
  maximizer::SoftKneeMaxConfig config;
  config.input_gain_db = f(params, "inputGainDb", config.input_gain_db);
  config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
  config.knee_db = f(params, "kneeDb", config.knee_db);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  return config;
}

inline maximizer::AdaptiveReleaseConfig adaptive_release_config(const ParamMap& params) {
  maximizer::AdaptiveReleaseConfig config;
  config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.min_release_ms = f(params, "minReleaseMs", config.min_release_ms);
  config.max_release_ms = f(params, "maxReleaseMs", config.max_release_ms);
  config.crest_window_ms = f(params, "crestWindowMs", config.crest_window_ms);
  config.crest_low = f(params, "crestLow", config.crest_low);
  config.crest_high = f(params, "crestHigh", config.crest_high);
  config.release_smoothing_ms = f(params, "releaseSmoothingMs", config.release_smoothing_ms);
  return config;
}

}  // namespace sonare::mastering::api::detail
