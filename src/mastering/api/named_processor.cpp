#include "mastering/api/named_processor.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "analysis/meter/lufs.h"
#include "core/audio.h"
#include "mastering/common/processor_base.h"
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
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/match/ab_switcher.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/match/tonal_balance.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
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
#include "mastering/stereo/mono_compat_check.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"

namespace sonare::mastering::api {
namespace {

using ParamMap = std::unordered_map<std::string, double>;

ParamMap make_map(const std::vector<Param>& params) {
  ParamMap map;
  for (const auto& param : params) {
    map[param.key] = param.value;
  }
  return map;
}

float f(const ParamMap& params, const char* key, float default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : static_cast<float>(it->second);
}

int i(const ParamMap& params, const char* key, int default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : static_cast<int>(std::lround(it->second));
}

bool b(const ParamMap& params, const char* key, bool default_value) {
  auto it = params.find(key);
  return it == params.end() ? default_value : it->second != 0.0;
}

std::vector<float> cutoffs(const ParamMap& params) {
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

eq::EqBandType eq_band_type(int value) {
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
    default:
      return eq::EqBandType::Peak;
  }
}

eq::EqBand eq_band(const ParamMap& params, const std::string& prefix) {
  eq::EqBand band;
  band.type = eq_band_type(i(params, (prefix + "type").c_str(), 0));
  band.frequency_hz = f(params, (prefix + "frequencyHz").c_str(), band.frequency_hz);
  band.gain_db = f(params, (prefix + "gainDb").c_str(), band.gain_db);
  band.q = f(params, (prefix + "q").c_str(), band.q);
  band.enabled = b(params, (prefix + "enabled").c_str(), true);
  return band;
}

void configure_parametric(eq::ParametricEq& processor, const ParamMap& params,
                          const std::string& prefix = "band") {
  for (size_t index = 0; index < eq::ParametricEq::kMaxBands; ++index) {
    const std::string band_prefix = prefix + std::to_string(index) + ".";
    if (params.find(band_prefix + "frequencyHz") != params.end() ||
        params.find(band_prefix + "gainDb") != params.end() ||
        params.find(band_prefix + "enabled") != params.end()) {
      processor.set_band(index, eq_band(params, band_prefix));
    }
  }
}

template <typename Processor>
void run_processor(Processor& processor, std::vector<float>& samples, int sample_rate,
                   int& latency_samples) {
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
  latency_samples = processor.latency_samples();
}

template <typename Processor>
void run_processor_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right,
                          int sample_rate, int& latency_samples) {
  processor.prepare(sample_rate, static_cast<int>(left.size()));
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
  latency_samples = processor.latency_samples();
}

float lufs_for(const std::vector<float>& samples, int sample_rate) {
  auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  return analysis::meter::lufs(audio).integrated_lufs;
}

std::vector<float> mono_mix(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> mono(left.size());
  for (size_t index = 0; index < left.size(); ++index) {
    mono[index] = 0.5f * (left[index] + right[index]);
  }
  return mono;
}

void apply_gain(std::vector<float>& left, std::vector<float>& right, float gain_db) {
  const float gain = std::pow(10.0f, gain_db / 20.0f);
  for (size_t index = 0; index < left.size(); ++index) {
    left[index] *= gain;
    right[index] *= gain;
  }
}

dynamics::CompressorConfig compressor_config(const ParamMap& params) {
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

dynamics::LimiterConfig limiter_config(const ParamMap& params) {
  dynamics::LimiterConfig config;
  config.threshold_db = f(params, "thresholdDb", config.threshold_db);
  config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
  config.release_ms = f(params, "releaseMs", config.release_ms);
  return config;
}

multiband::CrossoverConfig crossover_config(const ParamMap& params) {
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

void configure_processor(const std::string& name, const ParamMap& params,
                         std::vector<float>& samples, int sample_rate, int& latency_samples,
                         float& applied_gain_db) {
  if (name == "dynamics.brickwallLimiter") {
    dynamics::BrickwallLimiterConfig config;
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    dynamics::BrickwallLimiter p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.compressor") {
    dynamics::Compressor p(compressor_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.deesser") {
    dynamics::DeEsserConfig config;
    config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
    config.threshold_db = f(params, "thresholdDb", config.threshold_db);
    config.ratio = f(params, "ratio", config.ratio);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    dynamics::DeEsser p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.expander") {
    dynamics::ExpanderConfig config;
    config.threshold_db = f(params, "thresholdDb", config.threshold_db);
    config.ratio = f(params, "ratio", config.ratio);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    dynamics::Expander p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.gate") {
    dynamics::GateConfig config;
    config.threshold_db = f(params, "thresholdDb", config.threshold_db);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    config.hold_ms = f(params, "holdMs", config.hold_ms);
    config.close_threshold_db = f(params, "closeThresholdDb", config.close_threshold_db);
    config.key_hpf_hz = f(params, "keyHpfHz", config.key_hpf_hz);
    dynamics::Gate p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.limiter") {
    dynamics::Limiter p(limiter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.parallelComp") {
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
    dynamics::ParallelComp p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.sidechainRouter") {
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
    dynamics::SidechainRouter p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.transientShaper") {
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
    dynamics::TransientShaper p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.upwardCompressor") {
    dynamics::UpwardCompressorConfig config;
    config.threshold_db = f(params, "thresholdDb", config.threshold_db);
    config.ratio = f(params, "ratio", config.ratio);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    dynamics::UpwardCompressor p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.upwardExpander") {
    dynamics::UpwardExpanderConfig config;
    config.threshold_db = f(params, "thresholdDb", config.threshold_db);
    config.ratio = f(params, "ratio", config.ratio);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    dynamics::UpwardExpander p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.vocalRider") {
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
    dynamics::VocalRider p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.tilt") {
    eq::TiltEq p;
    p.set_tilt_db(f(params, "tiltDb", 0.0f));
    p.set_pivot_hz(f(params, "pivotHz", 1000.0f));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.apiStyle") {
    eq::ApiStyleEq p;
    p.set_band(eq::ApiStyleEq::Band::Low, f(params, "lowFrequencyHz", 100.0f),
               f(params, "lowGainDb", 0.0f));
    p.set_band(eq::ApiStyleEq::Band::LowMid, f(params, "lowMidFrequencyHz", 400.0f),
               f(params, "lowMidGainDb", 0.0f));
    p.set_band(eq::ApiStyleEq::Band::HighMid, f(params, "highMidFrequencyHz", 3000.0f),
               f(params, "highMidGainDb", 0.0f));
    p.set_band(eq::ApiStyleEq::Band::High, f(params, "highFrequencyHz", 10000.0f),
               f(params, "highGainDb", 0.0f));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.parametric") {
    eq::ParametricEq p;
    configure_parametric(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.minimumPhase") {
    eq::MinimumPhaseEq p;
    for (size_t index = 0; index < eq::MinimumPhaseEq::kMaxBands; ++index) {
      const std::string prefix = "band" + std::to_string(index) + ".";
      if (params.find(prefix + "frequencyHz") != params.end() ||
          params.find(prefix + "gainDb") != params.end()) {
        p.set_band(index, eq_band(params, prefix));
      }
    }
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.linearPhase") {
    eq::LinearPhaseEqConfig config;
    config.fft_size = i(params, "fftSize", config.fft_size);
    config.kernel_size = i(params, "kernelSize", config.kernel_size);
    config.use_partitioned_convolution =
        b(params, "usePartitionedConvolution", config.use_partitioned_convolution);
    config.partition_size = i(params, "partitionSize", config.partition_size);
    eq::LinearPhaseEq p(config);
    for (size_t index = 0; index < eq::LinearPhaseEq::kMaxBands; ++index) {
      const std::string prefix = "band" + std::to_string(index) + ".";
      if (params.find(prefix + "frequencyHz") != params.end() ||
          params.find(prefix + "gainDb") != params.end()) {
        p.set_band(index, eq_band(params, prefix));
      }
    }
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.dynamic") {
    eq::DynamicEq p;
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
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.pultec") {
    eq::PultecEq p;
    p.set_low_frequency(f(params, "lowFrequencyHz", 60.0f));
    p.set_low_boost(f(params, "lowBoost", 0.0f));
    p.set_low_attenuation(f(params, "lowAttenuation", 0.0f));
    p.set_high_boost(f(params, "highBoostFrequencyHz", 8000.0f), f(params, "highBoost", 0.0f),
                     f(params, "highBandwidth", 0.5f));
    p.set_high_attenuation(f(params, "highAttenuationFrequencyHz", 10000.0f),
                           f(params, "highAttenuation", 0.0f));
    p.set_component_model(static_cast<eq::PultecComponentModel>(i(params, "componentModel", 0)));
    p.set_output_drive(f(params, "outputDrive", 0.0f));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.cutFilter") {
    eq::CutFilter p;
    p.set_high_pass(f(params, "highPassFrequencyHz", 20.0f), f(params, "highPassQ", 0.707f),
                    static_cast<eq::CutFilterSlope>(i(params, "highPassSlope", 0)),
                    b(params, "highPassEnabled", false));
    p.set_low_pass(f(params, "lowPassFrequencyHz", 20000.0f), f(params, "lowPassQ", 0.707f),
                   static_cast<eq::CutFilterSlope>(i(params, "lowPassSlope", 0)),
                   b(params, "lowPassEnabled", false));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.bandPass") {
    eq::BandPassEq p;
    p.set_band_pass(f(params, "bandPassFrequencyHz", 1000.0f), f(params, "bandPassQ", 1.0f),
                    b(params, "bandPassEnabled", true));
    p.set_notch(f(params, "notchFrequencyHz", 1000.0f), f(params, "notchQ", 1.0f),
                b(params, "notchEnabled", false));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.shelving") {
    eq::ShelvingEq p;
    p.set_low_shelf(f(params, "lowFrequencyHz", 100.0f), f(params, "lowGainDb", 0.0f),
                    f(params, "lowQ", 0.707f), b(params, "lowEnabled", true));
    p.set_high_shelf(f(params, "highFrequencyHz", 10000.0f), f(params, "highGainDb", 0.0f),
                     f(params, "highQ", 0.707f), b(params, "highEnabled", true));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.graphic") {
    eq::GraphicEq p;
    for (size_t index = 0; index < eq::GraphicEq::kNumBands; ++index) {
      const std::string key = "band" + std::to_string(index) + "GainDb";
      if (params.find(key) != params.end()) p.set_gain_db(index, f(params, key.c_str(), 0.0f));
    }
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.maximizer") {
    maximizer::MaximizerConfig config;
    config.input_gain_db = f(params, "inputGainDb", config.input_gain_db);
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    maximizer::Maximizer p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.truePeakLimiter") {
    maximizer::TruePeakLimiterConfig config;
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.oversample_factor = i(params, "oversampleFactor", config.oversample_factor);
    config.apply_gain_at_input_rate =
        b(params, "applyGainAtInputRate", config.apply_gain_at_input_rate);
    maximizer::TruePeakLimiter p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.softKneeMax") {
    maximizer::SoftKneeMaxConfig config;
    config.input_gain_db = f(params, "inputGainDb", config.input_gain_db);
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.knee_db = f(params, "kneeDb", config.knee_db);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    maximizer::SoftKneeMax p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.adaptiveRelease") {
    maximizer::AdaptiveReleaseConfig config;
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.lookahead_ms = f(params, "lookaheadMs", config.lookahead_ms);
    config.min_release_ms = f(params, "minReleaseMs", config.min_release_ms);
    config.max_release_ms = f(params, "maxReleaseMs", config.max_release_ms);
    config.crest_window_ms = f(params, "crestWindowMs", config.crest_window_ms);
    config.crest_low = f(params, "crestLow", config.crest_low);
    config.crest_high = f(params, "crestHigh", config.crest_high);
    config.release_smoothing_ms = f(params, "releaseSmoothingMs", config.release_smoothing_ms);
    maximizer::AdaptiveRelease p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.loudnessOptimize") {
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    maximizer::LoudnessOptimizeConfig config;
    config.target_lufs = f(params, "targetLufs", config.target_lufs);
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.true_peak_oversample = i(params, "truePeakOversample", config.true_peak_oversample);
    auto result = maximizer::loudness_optimize(audio, config);
    samples.assign(result.audio.data(), result.audio.data() + result.audio.size());
    applied_gain_db += result.applied_gain_db;
  } else if (name == "saturation.tape") {
    saturation::TapeConfig config;
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.saturation = f(params, "saturation", config.saturation);
    config.hysteresis = f(params, "hysteresis", config.hysteresis);
    config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
    config.speed_ips = f(params, "speedIps", config.speed_ips);
    config.head_bump_db = f(params, "headBumpDb", config.head_bump_db);
    config.bias = f(params, "bias", config.bias);
    config.gap_loss = f(params, "gapLoss", config.gap_loss);
    saturation::Tape p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.exciter") {
    saturation::ExciterConfig config;
    config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.amount = f(params, "amount", config.amount);
    config.q = f(params, "q", config.q);
    config.even_odd_mix = f(params, "evenOddMix", config.even_odd_mix);
    saturation::Exciter p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.bitcrusher") {
    saturation::BitCrusherConfig config;
    config.bit_depth = i(params, "bitDepth", config.bit_depth);
    config.downsample_factor = i(params, "downsampleFactor", config.downsample_factor);
    config.mix = f(params, "mix", config.mix);
    config.dither_type = static_cast<final::DitherType>(i(params, "ditherType", 0));
    config.dither_seed = static_cast<uint32_t>(i(params, "ditherSeed", config.dither_seed));
    saturation::BitCrusher p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.hardClipper") {
    saturation::HardClipperConfig config;
    config.ceiling = f(params, "ceiling", config.ceiling);
    saturation::HardClipper p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.softClipper") {
    saturation::SoftClipperConfig config;
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.ceiling = f(params, "ceiling", config.ceiling);
    config.mix = f(params, "mix", config.mix);
    saturation::SoftClipper p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.waveshaper") {
    saturation::WaveshaperConfig config;
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.mix = f(params, "mix", config.mix);
    config.output_gain_db = f(params, "outputGainDb", config.output_gain_db);
    config.bias = f(params, "bias", config.bias);
    config.curve = static_cast<saturation::WaveshaperCurve>(i(params, "curve", 0));
    saturation::Waveshaper p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.tube") {
    saturation::TubeConfig config;
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.bias = f(params, "bias", config.bias);
    config.mix = f(params, "mix", config.mix);
    config.oversample_factor = i(params, "oversampleFactor", config.oversample_factor);
    config.bias_v = f(params, "biasV", config.bias_v);
    saturation::Tube p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.transformer") {
    saturation::TransformerConfig config;
    config.drive_db = f(params, "driveDb", config.drive_db);
    config.asymmetry = f(params, "asymmetry", config.asymmetry);
    config.mix = f(params, "mix", config.mix);
    saturation::Transformer p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.multibandExciter") {
    saturation::MultibandExciterConfig config;
    config.crossover = crossover_config(params);
    for (size_t index = 0; index < config.bands.size(); ++index) {
      const std::string prefix = "band" + std::to_string(index) + ".";
      config.bands[index].frequency_hz =
          f(params, (prefix + "frequencyHz").c_str(), config.bands[index].frequency_hz);
      config.bands[index].drive_db =
          f(params, (prefix + "driveDb").c_str(), config.bands[index].drive_db);
      config.bands[index].amount =
          f(params, (prefix + "amount").c_str(), config.bands[index].amount);
      config.bands[index].q = f(params, (prefix + "q").c_str(), config.bands[index].q);
      config.bands[index].even_odd_mix =
          f(params, (prefix + "evenOddMix").c_str(), config.bands[index].even_odd_mix);
    }
    saturation::MultibandExciter p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.airBand") {
    spectral::AirBandConfig config;
    config.amount = f(params, "amount", config.amount);
    config.shelf_frequency_hz = f(params, "shelfFrequencyHz", config.shelf_frequency_hz);
    config.dynamic_threshold_db = f(params, "dynamicThresholdDb", config.dynamic_threshold_db);
    config.dynamic_range_db = f(params, "dynamicRangeDb", config.dynamic_range_db);
    spectral::AirBand p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.lowEndFocus") {
    spectral::LowEndFocusConfig config;
    config.cutoff_hz = f(params, "cutoffHz", config.cutoff_hz);
    config.width = f(params, "width", config.width);
    config.subharmonic_amount = f(params, "subharmonicAmount", config.subharmonic_amount);
    config.transient_tightness = f(params, "transientTightness", config.transient_tightness);
    spectral::LowEndFocus p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.presenceEnhancer") {
    spectral::PresenceEnhancerConfig config;
    config.amount = f(params, "amount", config.amount);
    config.drive = f(params, "drive", config.drive);
    config.center_frequency_hz = f(params, "centerFrequencyHz", config.center_frequency_hz);
    config.q = f(params, "q", config.q);
    spectral::PresenceEnhancer p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.spectralShaper") {
    spectral::SpectralShaperConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.amount = f(params, "amount", config.amount);
    config.frequency_hz = f(params, "frequencyHz", config.frequency_hz);
    config.high_frequency_hz = f(params, "highFrequencyHz", config.high_frequency_hz);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.range_db = f(params, "rangeDb", config.range_db);
    spectral::SpectralShaper p(config);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "repair.declick") {
    repair::DeclickConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.neighbor_ratio = f(params, "neighborRatio", config.neighbor_ratio);
    config.max_click_samples =
        static_cast<size_t>(i(params, "maxClickSamples", config.max_click_samples));
    config.lpc_order = i(params, "lpcOrder", config.lpc_order);
    config.residual_ratio = f(params, "residualRatio", config.residual_ratio);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::declick(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.declip") {
    repair::DeclipConfig config;
    config.clip_threshold = f(params, "clipThreshold", config.clip_threshold);
    config.lpc_order = i(params, "lpcOrder", config.lpc_order);
    config.iterations = i(params, "iterations", config.iterations);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::declip(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.decrackle") {
    repair::DecrackleConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.mode = static_cast<repair::DecrackleMode>(i(params, "mode", 0));
    config.levels = i(params, "levels", config.levels);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::decrackle(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.dehum") {
    repair::DehumConfig config;
    config.fundamental_hz = f(params, "fundamentalHz", config.fundamental_hz);
    config.harmonics = i(params, "harmonics", config.harmonics);
    config.q = f(params, "q", config.q);
    config.adaptive = b(params, "adaptive", config.adaptive);
    config.search_range_hz = f(params, "searchRangeHz", config.search_range_hz);
    config.adaptation = f(params, "adaptation", config.adaptation);
    config.frame_size = i(params, "frameSize", config.frame_size);
    config.pll_bandwidth = f(params, "pllBandwidth", config.pll_bandwidth);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::dehum(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.denoiseClassical" || name == "repair.denoise") {
    repair::DenoiseClassicalConfig config;
    config.mode = static_cast<repair::DenoiseMode>(i(params, "mode", 0));
    config.noise_estimator =
        static_cast<repair::DenoiseNoiseEstimator>(i(params, "noiseEstimator", 0));
    config.n_fft = i(params, "nFft", config.n_fft);
    config.hop_length = i(params, "hopLength", config.hop_length);
    config.dd_alpha = f(params, "ddAlpha", config.dd_alpha);
    config.gain_floor = f(params, "gainFloor", config.gain_floor);
    config.over_subtraction = f(params, "overSubtraction", config.over_subtraction);
    config.spectral_floor = f(params, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        f(params, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain = b(params, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = b(params, "gainSmoothing", config.gain_smoothing);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::denoise_classical(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.dereverbClassical") {
    repair::DereverbClassicalConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.attenuation = f(params, "attenuation", config.attenuation);
    config.n_fft = i(params, "nFft", config.n_fft);
    config.hop_length = i(params, "hopLength", config.hop_length);
    config.t60_sec = f(params, "t60Sec", config.t60_sec);
    config.late_delay_ms = f(params, "lateDelayMs", config.late_delay_ms);
    config.over_subtraction = f(params, "overSubtraction", config.over_subtraction);
    config.spectral_floor = f(params, "spectralFloor", config.spectral_floor);
    config.wpe_enabled = b(params, "wpeEnabled", config.wpe_enabled);
    config.wpe_iterations = i(params, "wpeIterations", config.wpe_iterations);
    config.wpe_taps = i(params, "wpeTaps", config.wpe_taps);
    config.wpe_strength = f(params, "wpeStrength", config.wpe_strength);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::dereverb_classical(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.trimSilence") {
    repair::TrimSilenceConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.padding_samples =
        static_cast<size_t>(i(params, "paddingSamples", config.padding_samples));
    config.mode = static_cast<repair::TrimSilenceMode>(i(params, "mode", 0));
    config.gate_lufs = f(params, "gateLufs", config.gate_lufs);
    config.window_ms = f(params, "windowMs", config.window_ms);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::trim_silence(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.bitDepth") {
    final::BitDepthConfig config;
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.clamp = b(params, "clamp", config.clamp);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::bit_depth(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.dither") {
    final::DitherConfig config;
    config.type = static_cast<final::DitherType>(i(params, "type", 2));
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.seed = static_cast<uint32_t>(i(params, "seed", config.seed));
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::dither(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.outputChain") {
    final::OutputChainConfig config;
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.dither_type = static_cast<final::DitherType>(i(params, "ditherType", 2));
    config.clamp = b(params, "clamp", config.clamp);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::output_chain(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else {
    throw std::invalid_argument("unknown mastering processor: " + name);
  }
}

}  // namespace

std::vector<std::string> processor_names() {
  return {"dynamics.brickwallLimiter",
          "dynamics.compressor",
          "dynamics.deesser",
          "dynamics.expander",
          "dynamics.gate",
          "dynamics.limiter",
          "dynamics.parallelComp",
          "dynamics.sidechainRouter",
          "dynamics.transientShaper",
          "dynamics.upwardCompressor",
          "dynamics.upwardExpander",
          "dynamics.vocalRider",
          "eq.apiStyle",
          "eq.bandPass",
          "eq.cutFilter",
          "eq.dynamic",
          "eq.graphic",
          "eq.linearPhase",
          "eq.midSide",
          "eq.minimumPhase",
          "eq.parametric",
          "eq.pultec",
          "eq.shelving",
          "eq.tilt",
          "final.bitDepth",
          "final.dither",
          "final.outputChain",
          "maximizer.adaptiveRelease",
          "maximizer.loudnessOptimize",
          "maximizer.maximizer",
          "maximizer.softKneeMax",
          "maximizer.truePeakLimiter",
          "multiband.compressor",
          "multiband.dynamicEq",
          "multiband.expander",
          "multiband.imager",
          "multiband.limiter",
          "multiband.saturation",
          "repair.declick",
          "repair.declip",
          "repair.decrackle",
          "repair.dehum",
          "repair.denoiseClassical",
          "repair.dereverbClassical",
          "repair.trimSilence",
          "saturation.bitcrusher",
          "saturation.exciter",
          "saturation.hardClipper",
          "saturation.multibandExciter",
          "saturation.softClipper",
          "saturation.tape",
          "saturation.transformer",
          "saturation.tube",
          "saturation.waveshaper",
          "spectral.airBand",
          "spectral.lowEndFocus",
          "spectral.presenceEnhancer",
          "spectral.spectralShaper",
          "stereo.autoPan",
          "stereo.haasEnhancer",
          "stereo.imager",
          "stereo.monoMaker",
          "stereo.phaseAlign",
          "stereo.stereoBalance"};
}

MonoResult apply_named_processor(const std::string& name, const float* samples, std::size_t length,
                                 int sample_rate, const std::vector<Param>& params) {
  MonoResult result;
  result.samples.assign(samples, samples + length);
  result.sample_rate = sample_rate;
  result.input_lufs = lufs_for(result.samples, sample_rate);
  auto map = make_map(params);
  configure_processor(name, map, result.samples, sample_rate, result.latency_samples,
                      result.applied_gain_db);
  result.output_lufs = lufs_for(result.samples, sample_rate);
  return result;
}

StereoResult apply_named_processor_stereo(const std::string& name, const float* left,
                                          const float* right, std::size_t length, int sample_rate,
                                          const std::vector<Param>& params) {
  StereoResult result;
  result.left.assign(left, left + length);
  result.right.assign(right, right + length);
  result.sample_rate = sample_rate;
  result.input_lufs = lufs_for(mono_mix(result.left, result.right), sample_rate);
  auto map = make_map(params);

  if (name == "stereo.autoPan") {
    stereo::AutoPanConfig config;
    config.rate_hz = f(map, "rateHz", config.rate_hz);
    config.depth = f(map, "depth", config.depth);
    config.phase = f(map, "phase", config.phase);
    stereo::AutoPan p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.haasEnhancer") {
    stereo::HaasEnhancerConfig config;
    config.delay_ms = f(map, "delayMs", config.delay_ms);
    config.mix = f(map, "mix", config.mix);
    config.delay_right = b(map, "delayRight", config.delay_right);
    stereo::HaasEnhancer p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.imager") {
    stereo::ImagerConfig config;
    config.width = f(map, "width", config.width);
    config.output_gain_db = f(map, "outputGainDb", config.output_gain_db);
    config.decorrelation_amount = f(map, "decorrelationAmount", config.decorrelation_amount);
    config.preserve_energy = b(map, "preserveEnergy", config.preserve_energy);
    stereo::Imager p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.monoMaker") {
    stereo::MonoMakerConfig config;
    config.amount = f(map, "amount", config.amount);
    stereo::MonoMaker p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.phaseAlign") {
    stereo::PhaseAlignConfig config;
    config.delay_samples = i(map, "delaySamples", config.delay_samples);
    config.delay_right = b(map, "delayRight", config.delay_right);
    config.fractional_delay_samples =
        f(map, "fractionalDelaySamples", config.fractional_delay_samples);
    stereo::PhaseAlign p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.stereoBalance") {
    stereo::StereoBalanceConfig config;
    config.balance = f(map, "balance", config.balance);
    config.constant_power = b(map, "constantPower", config.constant_power);
    stereo::StereoBalance p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "eq.midSide") {
    eq::MidSideEq p;
    for (size_t index = 0; index < eq::MidSideEq::kMaxBands; ++index) {
      const std::string mid = "midBand" + std::to_string(index) + ".";
      const std::string side = "sideBand" + std::to_string(index) + ".";
      if (map.find(mid + "frequencyHz") != map.end() || map.find(mid + "gainDb") != map.end()) {
        p.set_mid_band(index, eq_band(map, mid));
      }
      if (map.find(side + "frequencyHz") != map.end() || map.find(side + "gainDb") != map.end()) {
        p.set_side_band(index, eq_band(map, side));
      }
    }
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.compressor") {
    multiband::MultibandCompressorConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandCompressor p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.expander") {
    multiband::MultibandExpanderConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandExpander p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.limiter") {
    multiband::MultibandLimiterConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandLimiter p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.imager") {
    multiband::MultibandImagerConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandImager p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.saturation") {
    multiband::MultibandSaturationConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandSaturation p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.dynamicEq") {
    multiband::MultibandDynamicEqConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandDynamicEq p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "maximizer.loudnessOptimize") {
    const float current = lufs_for(mono_mix(result.left, result.right), sample_rate);
    if (std::isfinite(current)) {
      const float gain_db = f(map, "targetLufs", -14.0f) - current;
      apply_gain(result.left, result.right, gain_db);
      result.applied_gain_db += gain_db;
    }
    maximizer::TruePeakLimiterConfig config;
    config.ceiling_db = f(map, "ceilingDb", config.ceiling_db);
    config.oversample_factor = i(map, "truePeakOversample", config.oversample_factor);
    config.apply_gain_at_input_rate =
        b(map, "applyGainAtInputRate", config.apply_gain_at_input_rate);
    maximizer::TruePeakLimiter p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else {
    std::vector<float> mono = mono_mix(result.left, result.right);
    configure_processor(name, map, mono, sample_rate, result.latency_samples,
                        result.applied_gain_db);
    for (size_t index = 0; index < result.left.size(); ++index) {
      const float old_mid = 0.5f * (result.left[index] + result.right[index]);
      const float delta = mono[index] - old_mid;
      result.left[index] += delta;
      result.right[index] += delta;
    }
  }

  result.output_lufs = lufs_for(mono_mix(result.left, result.right), sample_rate);
  return result;
}

std::vector<std::string> pair_processor_names() {
  return {"match.applyMatchEq", "match.alignReferenceToSource", "match.abSwitch",
          "match.abCrossfade"};
}

std::vector<std::string> pair_analysis_names() {
  return {"match.referenceLoudness", "match.tonalBalance", "match.tonalBalanceLogBands",
          "match.matchEqCurve", "match.estimateReferenceDelaySamples"};
}

std::vector<std::string> stereo_analysis_names() {
  return {"stereo.monoCompatCheck", "stereo.monoCompatCheckLogBands"};
}

MonoResult apply_named_pair_processor(const std::string& name, const float* source,
                                      const float* reference, std::size_t length, int sample_rate,
                                      const std::vector<Param>& params) {
  auto map = make_map(params);
  auto source_audio = Audio::from_buffer(source, length, sample_rate);
  auto reference_audio = Audio::from_buffer(reference, length, sample_rate);
  Audio out;
  if (name == "match.applyMatchEq") {
    ::sonare::mastering::match::MatchEqConfig match_config;
    match_config.max_bands = static_cast<size_t>(i(map, "maxBands", match_config.max_bands));
    match_config.max_gain_db = f(map, "maxGainDb", match_config.max_gain_db);
    match_config.min_frequency_hz = f(map, "minFrequencyHz", match_config.min_frequency_hz);
    match_config.max_frequency_hz = f(map, "maxFrequencyHz", match_config.max_frequency_hz);
    match_config.q = f(map, "q", match_config.q);
    match_config.smoothing_bins = i(map, "smoothingBins", match_config.smoothing_bins);
    ::sonare::mastering::match::MatchEqFirConfig fir_config;
    fir_config.fft_size = i(map, "fftSize", fir_config.fft_size);
    fir_config.kernel_size = i(map, "kernelSize", fir_config.kernel_size);
    fir_config.phase = static_cast<::sonare::mastering::match::MatchEqFirPhase>(i(map, "phase", 0));
    fir_config.partition_size = i(map, "partitionSize", fir_config.partition_size);
    auto source_spectrum = ::sonare::mastering::match::reference_spectrum(source_audio);
    auto reference_spectrum = ::sonare::mastering::match::reference_spectrum(reference_audio);
    out = ::sonare::mastering::match::apply_match_eq(source_audio, source_spectrum,
                                                     reference_spectrum, match_config, fir_config);
  } else if (name == "match.alignReferenceToSource") {
    out = ::sonare::mastering::match::align_reference_to_source(source_audio, reference_audio,
                                                                i(map, "maxAbsDelay", 4096));
  } else if (name == "match.abSwitch") {
    out = ::sonare::mastering::match::ab_switch(
        source_audio, reference_audio,
        static_cast<::sonare::mastering::match::ABSelection>(i(map, "selection", 0)));
  } else if (name == "match.abCrossfade") {
    out = ::sonare::mastering::match::ab_crossfade(source_audio, reference_audio,
                                                   f(map, "mix", 0.5f));
  } else {
    throw std::invalid_argument("unknown mastering pair processor: " + name);
  }

  MonoResult result;
  result.samples.assign(out.data(), out.data() + out.size());
  result.sample_rate = out.sample_rate();
  result.input_lufs = lufs_for(std::vector<float>(source, source + length), sample_rate);
  result.output_lufs = lufs_for(result.samples, result.sample_rate);
  return result;
}

std::string analyze_named_pair(const std::string& name, const float* source, const float* reference,
                               std::size_t length, int sample_rate,
                               const std::vector<Param>& params) {
  auto map = make_map(params);
  auto source_audio = Audio::from_buffer(source, length, sample_rate);
  auto reference_audio = Audio::from_buffer(reference, length, sample_rate);
  std::ostringstream json;
  json << "{";
  if (name == "match.referenceLoudness") {
    auto result = ::sonare::mastering::match::reference_loudness(source_audio, reference_audio);
    json << "\"sourceLufs\":" << result.source_lufs
         << ",\"referenceLufs\":" << result.reference_lufs
         << ",\"gainToMatchDb\":" << result.gain_to_match_db;
  } else if (name == "match.tonalBalance" || name == "match.tonalBalanceLogBands") {
    auto source_spectrum = ::sonare::mastering::match::reference_spectrum(source_audio);
    auto reference_spectrum = ::sonare::mastering::match::reference_spectrum(reference_audio);
    auto bands =
        name == "match.tonalBalance"
            ? ::sonare::mastering::match::tonal_balance(source_spectrum, reference_spectrum)
            : ::sonare::mastering::match::tonal_balance_log_bands(
                  source_spectrum, reference_spectrum, i(map, "bandsPerOctave", 3),
                  f(map, "lowHz", 20.0f), f(map, "highHz", 20000.0f));
    json << "\"bands\":[";
    for (size_t index = 0; index < bands.size(); ++index) {
      if (index > 0) json << ",";
      const auto& band = bands[index];
      json << "{\"lowHz\":" << band.low_hz << ",\"highHz\":" << band.high_hz
           << ",\"sourceDb\":" << band.source_db << ",\"referenceDb\":" << band.reference_db
           << ",\"deviationDb\":" << band.deviation_db << "}";
    }
    json << "]";
  } else if (name == "match.matchEqCurve") {
    ::sonare::mastering::match::MatchEqConfig config;
    config.max_bands = static_cast<size_t>(i(map, "maxBands", config.max_bands));
    config.max_gain_db = f(map, "maxGainDb", config.max_gain_db);
    config.min_frequency_hz = f(map, "minFrequencyHz", config.min_frequency_hz);
    config.max_frequency_hz = f(map, "maxFrequencyHz", config.max_frequency_hz);
    config.q = f(map, "q", config.q);
    config.smoothing_bins = i(map, "smoothingBins", config.smoothing_bins);
    auto curve = ::sonare::mastering::match::match_eq_curve(
        ::sonare::mastering::match::reference_spectrum(source_audio),
        ::sonare::mastering::match::reference_spectrum(reference_audio), config);
    json << "\"frequencies\":[";
    for (size_t index = 0; index < curve.frequencies.size(); ++index) {
      if (index > 0) json << ",";
      json << curve.frequencies[index];
    }
    json << "],\"gainDb\":[";
    for (size_t index = 0; index < curve.gain_db.size(); ++index) {
      if (index > 0) json << ",";
      json << curve.gain_db[index];
    }
    json << "]";
  } else if (name == "match.estimateReferenceDelaySamples") {
    const float delay = ::sonare::mastering::match::estimate_reference_delay_samples(
        source_audio, reference_audio, i(map, "maxAbsDelay", 4096));
    json << "\"delaySamples\":" << delay;
  } else {
    throw std::invalid_argument("unknown mastering pair analysis: " + name);
  }
  json << "}";
  return json.str();
}

std::string analyze_named_stereo(const std::string& name, const float* left, const float* right,
                                 std::size_t length, int sample_rate,
                                 const std::vector<Param>& params) {
  auto map = make_map(params);
  std::ostringstream json;
  json << "{";
  if (name == "stereo.monoCompatCheck") {
    auto result = ::sonare::mastering::stereo::mono_compat_check(
        left, right, length, f(map, "correlationThreshold", 0.0f));
    json << "\"correlation\":" << result.correlation << ",\"width\":" << result.width
         << ",\"monoPeak\":" << result.mono_peak << ",\"sideRms\":" << result.side_rms
         << ",\"likelyMonoCompatible\":" << (result.likely_mono_compatible ? "true" : "false");
  } else if (name == "stereo.monoCompatCheckLogBands") {
    auto bands = ::sonare::mastering::stereo::mono_compat_check_log_bands(
        left, right, length, sample_rate, i(map, "bandsPerOctave", 3), f(map, "lowHz", 20.0f),
        f(map, "highHz", 20000.0f));
    json << "\"bands\":[";
    for (size_t index = 0; index < bands.size(); ++index) {
      if (index > 0) json << ",";
      const auto& band = bands[index];
      json << "{\"lowHz\":" << band.low_hz << ",\"highHz\":" << band.high_hz
           << ",\"correlation\":" << band.correlation << ",\"sideRms\":" << band.side_rms << "}";
    }
    json << "]";
  } else {
    throw std::invalid_argument("unknown mastering stereo analysis: " + name);
  }
  json << "}";
  return json.str();
}

}  // namespace sonare::mastering::api
