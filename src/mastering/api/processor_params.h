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
#include "mastering/api/param_field_tables.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/ducking_processor.h"
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
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
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

/// @brief Overlays a flat param onto a config field, leaving it untouched when
/// the key is absent. Paired with the SONARE_FIELDS_* tables so a config
/// builder is a single table expansion instead of one line per field.
template <typename T>
inline void read_field(const ParamMap& params, const char* key, T& dst) {
  auto it = params.find(key);
  if (it != params.end()) assign_field(dst, it->second);
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
  processor.set_gain_scale(f(params, "gainScale", processor.gain_scale()));
  processor.set_output_gain_db(f(params, "outputGainDb", processor.output_gain_db()));
  processor.set_output_pan(f(params, "outputPan", processor.output_pan()));
  processor.set_phase_mode(
      phase_mode(i(params, "phaseMode", static_cast<int>(processor.phase_mode()))));
  for (size_t index = 0; index < eq::EqualizerProcessor::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    if (has_eq_band_params(params, band_prefix)) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

// Expands one SONARE_FIELDS_* table row into a field overlay. A config builder
// is then just `Config config; SONARE_FIELDS_X(SONARE_READ_FIELD); return ...`,
// equivalent to the prior per-field `config.x = f(params, "x", config.x)` lines.
#define SONARE_READ_FIELD(key, member) read_field(params, key, config.member);

inline dynamics::CompressorConfig compressor_config(const ParamMap& params) {
  dynamics::CompressorConfig config;
  SONARE_FIELDS_COMPRESSOR(SONARE_READ_FIELD)
  return config;
}

inline dynamics::LimiterConfig limiter_config(const ParamMap& params) {
  dynamics::LimiterConfig config;
  SONARE_FIELDS_LIMITER(SONARE_READ_FIELD)
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
// Multiband per-band population
//
// Each multiband processor exposes its sub-bands through a `band{i}.<field>`
// flat-key convention (matching multiband_exciter_config()). The crossover
// config decides how many bands exist; these helpers iterate the already-sized
// `bands` vector and overlay any caller-supplied per-band fields onto the
// factory defaults so they are not silently ignored.
// ---------------------------------------------------------------------------

inline void populate_compressor_bands(multiband::MultibandCompressorConfig& config,
                                      const ParamMap& params) {
  for (size_t index = 0; index < config.bands.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    auto& band = config.bands[index];
    band.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.threshold_db);
    band.ratio = f(params, (prefix + "ratio").c_str(), band.ratio);
    band.attack_ms = f(params, (prefix + "attackMs").c_str(), band.attack_ms);
    band.release_ms = f(params, (prefix + "releaseMs").c_str(), band.release_ms);
    band.knee_db = f(params, (prefix + "kneeDb").c_str(), band.knee_db);
    band.makeup_gain_db = f(params, (prefix + "makeupGainDb").c_str(), band.makeup_gain_db);
  }
}

inline void populate_expander_bands(multiband::MultibandExpanderConfig& config,
                                    const ParamMap& params) {
  for (size_t index = 0; index < config.bands.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    auto& band = config.bands[index];
    band.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.threshold_db);
    band.ratio = f(params, (prefix + "ratio").c_str(), band.ratio);
    band.attack_ms = f(params, (prefix + "attackMs").c_str(), band.attack_ms);
    band.release_ms = f(params, (prefix + "releaseMs").c_str(), band.release_ms);
    band.range_db = f(params, (prefix + "rangeDb").c_str(), band.range_db);
  }
}

inline void populate_limiter_bands(multiband::MultibandLimiterConfig& config,
                                   const ParamMap& params) {
  for (size_t index = 0; index < config.bands.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    auto& band = config.bands[index];
    band.threshold_db = f(params, (prefix + "thresholdDb").c_str(), band.threshold_db);
    band.lookahead_ms = f(params, (prefix + "lookaheadMs").c_str(), band.lookahead_ms);
    band.release_ms = f(params, (prefix + "releaseMs").c_str(), band.release_ms);
  }
}

inline void populate_saturation_bands(multiband::MultibandSaturationConfig& config,
                                      const ParamMap& params) {
  for (size_t index = 0; index < config.bands.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    auto& band = config.bands[index];
    band.drive_db = f(params, (prefix + "driveDb").c_str(), band.drive_db);
    band.mix = f(params, (prefix + "mix").c_str(), band.mix);
    band.output_gain_db = f(params, (prefix + "outputGainDb").c_str(), band.output_gain_db);
  }
}

// ---------------------------------------------------------------------------
// Dynamics
// ---------------------------------------------------------------------------

inline dynamics::BrickwallLimiterConfig brickwall_limiter_config(const ParamMap& params) {
  dynamics::BrickwallLimiterConfig config;
  SONARE_FIELDS_BRICKWALL_LIMITER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::DeEsserConfig deesser_config(const ParamMap& params) {
  dynamics::DeEsserConfig config;
  SONARE_FIELDS_DEESSER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::ExpanderConfig expander_config(const ParamMap& params) {
  dynamics::ExpanderConfig config;
  SONARE_FIELDS_EXPANDER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::GateConfig gate_config(const ParamMap& params) {
  dynamics::GateConfig config;
  SONARE_FIELDS_GATE(SONARE_READ_FIELD)
  return config;
}

inline dynamics::ParallelCompConfig parallel_comp_config(const ParamMap& params) {
  dynamics::ParallelCompConfig config;
  SONARE_FIELDS_PARALLEL_COMP(SONARE_READ_FIELD)
  return config;
}

inline dynamics::SidechainRouterConfig sidechain_router_config(const ParamMap& params) {
  dynamics::SidechainRouterConfig config;
  SONARE_FIELDS_SIDECHAIN_ROUTER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::DuckingConfig ducking_config(const ParamMap& params) {
  dynamics::DuckingConfig config;
  SONARE_FIELDS_DUCKING(SONARE_READ_FIELD)
  return config;
}

inline dynamics::TransientShaperConfig transient_shaper_config(const ParamMap& params) {
  dynamics::TransientShaperConfig config;
  SONARE_FIELDS_TRANSIENT_SHAPER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::UpwardCompressorConfig upward_compressor_config(const ParamMap& params) {
  dynamics::UpwardCompressorConfig config;
  SONARE_FIELDS_UPWARD_COMPRESSOR(SONARE_READ_FIELD)
  return config;
}

inline dynamics::UpwardExpanderConfig upward_expander_config(const ParamMap& params) {
  dynamics::UpwardExpanderConfig config;
  SONARE_FIELDS_UPWARD_EXPANDER(SONARE_READ_FIELD)
  return config;
}

inline dynamics::VocalRiderConfig vocal_rider_config(const ParamMap& params) {
  dynamics::VocalRiderConfig config;
  SONARE_FIELDS_VOCAL_RIDER(SONARE_READ_FIELD)
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
  SONARE_FIELDS_TAPE(SONARE_READ_FIELD)
  return config;
}

inline saturation::ExciterConfig exciter_config(const ParamMap& params) {
  saturation::ExciterConfig config;
  SONARE_FIELDS_EXCITER(SONARE_READ_FIELD)
  return config;
}

inline saturation::BitCrusherConfig bitcrusher_config(const ParamMap& params) {
  saturation::BitCrusherConfig config;
  SONARE_FIELDS_BITCRUSHER(SONARE_READ_FIELD)
  return config;
}

inline saturation::HardClipperConfig hard_clipper_config(const ParamMap& params) {
  saturation::HardClipperConfig config;
  SONARE_FIELDS_HARD_CLIPPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::SoftClipperConfig soft_clipper_config(const ParamMap& params) {
  saturation::SoftClipperConfig config;
  SONARE_FIELDS_SOFT_CLIPPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::WaveshaperConfig waveshaper_config(const ParamMap& params) {
  saturation::WaveshaperConfig config;
  SONARE_FIELDS_WAVESHAPER(SONARE_READ_FIELD)
  return config;
}

inline saturation::TubeConfig tube_config(const ParamMap& params) {
  saturation::TubeConfig config;
  SONARE_FIELDS_TUBE(SONARE_READ_FIELD)
  return config;
}

inline saturation::TransformerConfig transformer_config(const ParamMap& params) {
  saturation::TransformerConfig config;
  SONARE_FIELDS_TRANSFORMER(SONARE_READ_FIELD)
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
  SONARE_FIELDS_AIR_BAND(SONARE_READ_FIELD)
  return config;
}

inline spectral::LowEndFocusConfig low_end_focus_config(const ParamMap& params) {
  spectral::LowEndFocusConfig config;
  SONARE_FIELDS_LOW_END_FOCUS(SONARE_READ_FIELD)
  return config;
}

inline spectral::PresenceEnhancerConfig presence_enhancer_config(const ParamMap& params) {
  spectral::PresenceEnhancerConfig config;
  SONARE_FIELDS_PRESENCE_ENHANCER(SONARE_READ_FIELD)
  return config;
}

inline spectral::SpectralShaperConfig spectral_shaper_config(const ParamMap& params) {
  spectral::SpectralShaperConfig config;
  SONARE_FIELDS_SPECTRAL_SHAPER(SONARE_READ_FIELD)
  return config;
}

// ---------------------------------------------------------------------------
// Stereo
// ---------------------------------------------------------------------------

inline stereo::AutoPanConfig auto_pan_config(const ParamMap& params) {
  stereo::AutoPanConfig config;
  SONARE_FIELDS_AUTO_PAN(SONARE_READ_FIELD)
  return config;
}

inline stereo::HaasEnhancerConfig haas_enhancer_config(const ParamMap& params) {
  stereo::HaasEnhancerConfig config;
  SONARE_FIELDS_HAAS_ENHANCER(SONARE_READ_FIELD)
  return config;
}

inline stereo::ImagerConfig imager_config(const ParamMap& params) {
  stereo::ImagerConfig config;
  SONARE_FIELDS_IMAGER(SONARE_READ_FIELD)
  SONARE_CHECK_RANGE("stereo.imager.width", config.width, 0.0f, 2.0f);
  SONARE_CHECK_RANGE("stereo.imager.decorrelationAmount", config.decorrelation_amount, 0.0f, 1.0f);
  return config;
}

inline stereo::MonoMakerConfig mono_maker_config(const ParamMap& params) {
  stereo::MonoMakerConfig config;
  SONARE_FIELDS_MONO_MAKER(SONARE_READ_FIELD)
  return config;
}

inline stereo::PhaseAlignConfig phase_align_config(const ParamMap& params) {
  stereo::PhaseAlignConfig config;
  SONARE_FIELDS_PHASE_ALIGN(SONARE_READ_FIELD)
  return config;
}

inline stereo::StereoBalanceConfig stereo_balance_config(const ParamMap& params) {
  stereo::StereoBalanceConfig config;
  SONARE_FIELDS_STEREO_BALANCE(SONARE_READ_FIELD)
  return config;
}

// ---------------------------------------------------------------------------
// Maximizer
// ---------------------------------------------------------------------------

inline maximizer::MaximizerConfig maximizer_config(const ParamMap& params) {
  maximizer::MaximizerConfig config;
  SONARE_FIELDS_MAXIMIZER(SONARE_READ_FIELD)
  return config;
}

inline maximizer::TruePeakLimiterConfig true_peak_limiter_config(const ParamMap& params) {
  maximizer::TruePeakLimiterConfig config;
  SONARE_FIELDS_TRUE_PEAK_LIMITER(SONARE_READ_FIELD)
  return config;
}

inline maximizer::SoftKneeMaxConfig soft_knee_max_config(const ParamMap& params) {
  maximizer::SoftKneeMaxConfig config;
  SONARE_FIELDS_SOFT_KNEE_MAX(SONARE_READ_FIELD)
  return config;
}

inline maximizer::AdaptiveReleaseConfig adaptive_release_config(const ParamMap& params) {
  maximizer::AdaptiveReleaseConfig config;
  SONARE_FIELDS_ADAPTIVE_RELEASE(SONARE_READ_FIELD)
  return config;
}

}  // namespace sonare::mastering::api::detail
