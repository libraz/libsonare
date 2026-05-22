/// @file chain.cpp
/// @brief Implementation of the high-level mastering chain composition.

#include "mastering/api/chain.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/meter/lufs.h"
#include "core/audio.h"
#include "mastering/common/processor_base.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"

namespace sonare::mastering::api {
namespace {

// ---------------------------------------------------------------------------
// Shared per-processor helpers (mirror src/wasm/bindings.cpp lines 137-182).
// ---------------------------------------------------------------------------

void run_processor_mono(common::ProcessorBase& processor, std::vector<float>& samples,
                        int sample_rate) {
  if (samples.empty()) {
    return;
  }
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
}

void run_processor_stereo(common::ProcessorBase& processor, std::vector<float>& left,
                          std::vector<float>& right, int sample_rate) {
  if (left.empty()) {
    return;
  }
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  processor.prepare(sample_rate, static_cast<int>(left.size()));
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
}

std::vector<float> mono_mix(const std::vector<float>& left, const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  std::vector<float> mono(left.size());
  for (std::size_t i = 0; i < left.size(); ++i) {
    mono[i] = 0.5f * (left[i] + right[i]);
  }
  return mono;
}

float integrated_lufs(const std::vector<float>& samples, int sample_rate) {
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  return analysis::meter::lufs(audio).integrated_lufs;
}

void apply_gain_db(std::vector<float>& left, std::vector<float>& right, float gain_db) {
  const float gain = std::pow(10.0f, gain_db / 20.0f);
  for (std::size_t i = 0; i < left.size(); ++i) {
    left[i] *= gain;
    right[i] *= gain;
  }
}

// ---------------------------------------------------------------------------
// Count of enabled stages for progress callback denominator.
// ---------------------------------------------------------------------------

int count_enabled_mono_stages(const MasteringChainConfig& cfg) {
  int n = 0;
  if (cfg.repair.denoise.enabled) ++n;
  if (cfg.eq.tilt.enabled) ++n;
  if (cfg.dynamics.compressor.enabled) ++n;
  if (cfg.saturation.tape.enabled) ++n;
  if (cfg.saturation.exciter.enabled) ++n;
  if (cfg.spectral.air_band.enabled) ++n;
  if (cfg.maximizer.true_peak_limiter.enabled) ++n;
  if (cfg.loudness.enabled) ++n;
  return n;
}

int count_enabled_stereo_stages(const MasteringChainConfig& cfg) {
  int n = count_enabled_mono_stages(cfg);
  if (cfg.stereo.imager.enabled) ++n;
  if (cfg.stereo.mono_maker.enabled) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Flat-params helpers
// ---------------------------------------------------------------------------

struct StageFlags {
  bool any_key_seen = false;
  bool enabled_explicit = false;
  bool enabled_value = false;
};

void mark_field(StageFlags& flags) { flags.any_key_seen = true; }

void mark_enabled(StageFlags& flags, double value) {
  flags.any_key_seen = true;
  flags.enabled_explicit = true;
  flags.enabled_value = value != 0.0;
}

bool resolve_enabled(const StageFlags& flags) {
  if (flags.enabled_explicit) {
    return flags.enabled_value;
  }
  return flags.any_key_seen;
}

}  // namespace

// ---------------------------------------------------------------------------
// MasteringChain
// ---------------------------------------------------------------------------

MasteringChain::MasteringChain(MasteringChainConfig config) : config_(std::move(config)) {}

void MasteringChain::set_progress_callback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
}

MonoChainResult MasteringChain::process_mono(const float* samples, std::size_t length,
                                             int sample_rate) {
  MonoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> data(samples, samples + length);
  result.input_lufs = integrated_lufs(data, sample_rate);
  float applied_gain_db = 0.0f;

  const int total = count_enabled_mono_stages(config_);
  int done = 0;
  auto report = [&](const char* stage_name) {
    result.stages.emplace_back(stage_name);
    ++done;
    if (progress_callback_ && total > 0) {
      progress_callback_(static_cast<float>(done) / static_cast<float>(total), stage_name);
    }
  };

  // 1. repair.denoise
  if (config_.repair.denoise.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::denoise_classical(input, config_.repair.denoise.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.denoise");
  }

  // 2. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_mono(tilt, data, sample_rate);
    report("eq.tilt");
  }

  // 3. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_mono(processor, data, sample_rate);
    report("dynamics.compressor");
  }

  // 4. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_mono(processor, data, sample_rate);
    report("saturation.tape");
  }

  // 5. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_mono(processor, data, sample_rate);
    report("saturation.exciter");
  }

  // 6. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_mono(processor, data, sample_rate);
    report("spectral.airBand");
  }

  // 7. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_mono(processor, data, sample_rate);
    report("maximizer.truePeakLimiter");
  }

  // 8. loudness (mono uses loudness_optimize)
  if (config_.loudness.enabled) {
    Audio current = Audio::from_buffer(data.data(), data.size(), sample_rate);
    mastering::maximizer::LoudnessOptimizeConfig loudness_config;
    loudness_config.target_lufs = config_.loudness.target_lufs;
    loudness_config.ceiling_db = config_.loudness.ceiling_db;
    loudness_config.true_peak_oversample = config_.loudness.true_peak_oversample;
    auto loud = mastering::maximizer::loudness_optimize(current, loudness_config);
    data.assign(loud.audio.data(), loud.audio.data() + loud.audio.size());
    applied_gain_db += loud.applied_gain_db;
    report("loudness.optimize");
  }

  result.output_lufs = integrated_lufs(data, sample_rate);
  result.applied_gain_db = applied_gain_db;
  result.samples = std::move(data);
  return result;
}

StereoChainResult MasteringChain::process_stereo(const float* left_in, const float* right_in,
                                                 std::size_t length, int sample_rate) {
  StereoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> left(left_in, left_in + length);
  std::vector<float> right(right_in, right_in + length);

  result.input_lufs = integrated_lufs(mono_mix(left, right), sample_rate);
  float applied_gain_db = 0.0f;

  const int total = count_enabled_stereo_stages(config_);
  int done = 0;
  auto report = [&](const char* stage_name) {
    result.stages.emplace_back(stage_name);
    ++done;
    if (progress_callback_ && total > 0) {
      progress_callback_(static_cast<float>(done) / static_cast<float>(total), stage_name);
    }
  };

  // 1. repair.denoise (per-channel)
  if (config_.repair.denoise.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired =
        mastering::repair::denoise_classical(left_audio, config_.repair.denoise.config);
    Audio right_repaired =
        mastering::repair::denoise_classical(right_audio, config_.repair.denoise.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.denoise");
  }

  // 2. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_stereo(tilt, left, right, sample_rate);
    report("eq.tilt");
  }

  // 3. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("dynamics.compressor");
  }

  // 4. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("saturation.tape");
  }

  // 5. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("saturation.exciter");
  }

  // 6. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("spectral.airBand");
  }

  // 7. stereo.imager
  if (config_.stereo.imager.enabled) {
    mastering::stereo::Imager processor(config_.stereo.imager.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("stereo.imager");
  }

  // 8. stereo.monoMaker
  if (config_.stereo.mono_maker.enabled) {
    mastering::stereo::MonoMaker processor(config_.stereo.mono_maker.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("stereo.monoMaker");
  }

  // 9. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("maximizer.truePeakLimiter");
  }

  // 10. loudness (stereo path: manual gain + TruePeakLimiter pass)
  if (config_.loudness.enabled) {
    const float current_lufs = integrated_lufs(mono_mix(left, right), sample_rate);
    if (std::isfinite(current_lufs)) {
      const float gain_db = config_.loudness.target_lufs - current_lufs;
      apply_gain_db(left, right, gain_db);
      applied_gain_db += gain_db;
    }
    mastering::maximizer::TruePeakLimiterConfig limiter_config;
    limiter_config.ceiling_db = config_.loudness.ceiling_db;
    limiter_config.oversample_factor = config_.loudness.true_peak_oversample;
    limiter_config.release_ms = config_.loudness.release_ms;
    limiter_config.apply_gain_at_input_rate = config_.loudness.apply_gain_at_input_rate;
    mastering::maximizer::TruePeakLimiter processor(limiter_config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("loudness.optimize");
  }

  result.output_lufs = integrated_lufs(mono_mix(left, right), sample_rate);
  result.applied_gain_db = applied_gain_db;
  result.left = std::move(left);
  result.right = std::move(right);
  return result;
}

// ---------------------------------------------------------------------------
// Per-key dispatch shared by parse_chain_config_params and
// apply_chain_config_overrides.
// ---------------------------------------------------------------------------

namespace {

struct StageFlagsSet {
  StageFlags denoise;
  StageFlags tilt;
  StageFlags compressor;
  StageFlags tape;
  StageFlags exciter;
  StageFlags air_band;
  StageFlags imager;
  StageFlags mono_maker;
  StageFlags true_peak;
  StageFlags loudness;
};

void apply_one_param_to_config(MasteringChainConfig& cfg, const std::string& key, double v,
                               StageFlagsSet& flags) {
  const float vf = static_cast<float>(v);
  const int vi = static_cast<int>(v);

  // ---- repair.denoise ----
  if (key == "repair.denoise.enabled") {
    mark_enabled(flags.denoise, v);
  } else if (key == "repair.denoise.nFft") {
    cfg.repair.denoise.config.n_fft = vi;
    mark_field(flags.denoise);
  } else if (key == "repair.denoise.hopLength") {
    cfg.repair.denoise.config.hop_length = vi;
    mark_field(flags.denoise);
  } else if (key == "repair.denoise.ddAlpha") {
    cfg.repair.denoise.config.dd_alpha = vf;
    mark_field(flags.denoise);
  } else if (key == "repair.denoise.gainFloor") {
    cfg.repair.denoise.config.gain_floor = vf;
    mark_field(flags.denoise);

    // ---- eq.tilt ----
  } else if (key == "eq.tilt.enabled") {
    mark_enabled(flags.tilt, v);
  } else if (key == "eq.tilt.tiltDb") {
    cfg.eq.tilt.tilt_db = vf;
    mark_field(flags.tilt);
  } else if (key == "eq.tilt.pivotHz") {
    cfg.eq.tilt.pivot_hz = vf;
    mark_field(flags.tilt);

    // ---- dynamics.compressor ----
  } else if (key == "dynamics.compressor.enabled") {
    mark_enabled(flags.compressor, v);
  } else if (key == "dynamics.compressor.thresholdDb") {
    cfg.dynamics.compressor.config.threshold_db = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.ratio") {
    cfg.dynamics.compressor.config.ratio = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.attackMs") {
    cfg.dynamics.compressor.config.attack_ms = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.releaseMs") {
    cfg.dynamics.compressor.config.release_ms = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.kneeDb") {
    cfg.dynamics.compressor.config.knee_db = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.makeupGainDb") {
    cfg.dynamics.compressor.config.makeup_gain_db = vf;
    mark_field(flags.compressor);
  } else if (key == "dynamics.compressor.autoMakeup") {
    cfg.dynamics.compressor.config.auto_makeup = v != 0.0;
    mark_field(flags.compressor);

    // ---- saturation.tape ----
  } else if (key == "saturation.tape.enabled") {
    mark_enabled(flags.tape, v);
  } else if (key == "saturation.tape.driveDb") {
    cfg.saturation.tape.config.drive_db = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.saturation") {
    cfg.saturation.tape.config.saturation = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.hysteresis") {
    cfg.saturation.tape.config.hysteresis = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.outputGainDb") {
    cfg.saturation.tape.config.output_gain_db = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.speedIps") {
    cfg.saturation.tape.config.speed_ips = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.headBumpDb") {
    cfg.saturation.tape.config.head_bump_db = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.bias") {
    cfg.saturation.tape.config.bias = vf;
    mark_field(flags.tape);
  } else if (key == "saturation.tape.gapLoss") {
    cfg.saturation.tape.config.gap_loss = vf;
    mark_field(flags.tape);

    // ---- saturation.exciter ----
  } else if (key == "saturation.exciter.enabled") {
    mark_enabled(flags.exciter, v);
  } else if (key == "saturation.exciter.frequencyHz") {
    cfg.saturation.exciter.config.frequency_hz = vf;
    mark_field(flags.exciter);
  } else if (key == "saturation.exciter.driveDb") {
    cfg.saturation.exciter.config.drive_db = vf;
    mark_field(flags.exciter);
  } else if (key == "saturation.exciter.amount") {
    cfg.saturation.exciter.config.amount = vf;
    mark_field(flags.exciter);
  } else if (key == "saturation.exciter.q") {
    cfg.saturation.exciter.config.q = vf;
    mark_field(flags.exciter);
  } else if (key == "saturation.exciter.evenOddMix") {
    cfg.saturation.exciter.config.even_odd_mix = vf;
    mark_field(flags.exciter);

    // ---- spectral.airBand ----
  } else if (key == "spectral.airBand.enabled") {
    mark_enabled(flags.air_band, v);
  } else if (key == "spectral.airBand.amount") {
    cfg.spectral.air_band.config.amount = vf;
    mark_field(flags.air_band);
  } else if (key == "spectral.airBand.shelfFrequencyHz") {
    cfg.spectral.air_band.config.shelf_frequency_hz = vf;
    mark_field(flags.air_band);
  } else if (key == "spectral.airBand.dynamicThresholdDb") {
    cfg.spectral.air_band.config.dynamic_threshold_db = vf;
    mark_field(flags.air_band);
  } else if (key == "spectral.airBand.dynamicRangeDb") {
    cfg.spectral.air_band.config.dynamic_range_db = vf;
    mark_field(flags.air_band);

    // ---- stereo.imager ----
  } else if (key == "stereo.imager.enabled") {
    mark_enabled(flags.imager, v);
  } else if (key == "stereo.imager.width") {
    cfg.stereo.imager.config.width = vf;
    mark_field(flags.imager);
  } else if (key == "stereo.imager.outputGainDb") {
    cfg.stereo.imager.config.output_gain_db = vf;
    mark_field(flags.imager);
  } else if (key == "stereo.imager.decorrelationAmount") {
    cfg.stereo.imager.config.decorrelation_amount = vf;
    mark_field(flags.imager);
  } else if (key == "stereo.imager.preserveEnergy") {
    cfg.stereo.imager.config.preserve_energy = v != 0.0;
    mark_field(flags.imager);

    // ---- stereo.monoMaker ----
  } else if (key == "stereo.monoMaker.enabled") {
    mark_enabled(flags.mono_maker, v);
  } else if (key == "stereo.monoMaker.amount") {
    cfg.stereo.mono_maker.config.amount = vf;
    mark_field(flags.mono_maker);

    // ---- maximizer.truePeakLimiter ----
  } else if (key == "maximizer.truePeakLimiter.enabled") {
    mark_enabled(flags.true_peak, v);
  } else if (key == "maximizer.truePeakLimiter.ceilingDb") {
    cfg.maximizer.true_peak_limiter.config.ceiling_db = vf;
    mark_field(flags.true_peak);
  } else if (key == "maximizer.truePeakLimiter.lookaheadMs") {
    cfg.maximizer.true_peak_limiter.config.lookahead_ms = vf;
    mark_field(flags.true_peak);
  } else if (key == "maximizer.truePeakLimiter.releaseMs") {
    cfg.maximizer.true_peak_limiter.config.release_ms = vf;
    mark_field(flags.true_peak);
  } else if (key == "maximizer.truePeakLimiter.oversampleFactor") {
    cfg.maximizer.true_peak_limiter.config.oversample_factor = vi;
    mark_field(flags.true_peak);
  } else if (key == "maximizer.truePeakLimiter.applyGainAtInputRate") {
    cfg.maximizer.true_peak_limiter.config.apply_gain_at_input_rate = v != 0.0;
    mark_field(flags.true_peak);

    // ---- loudness ----
  } else if (key == "loudness.enabled") {
    mark_enabled(flags.loudness, v);
  } else if (key == "loudness.targetLufs") {
    cfg.loudness.target_lufs = vf;
    mark_field(flags.loudness);
  } else if (key == "loudness.ceilingDb") {
    cfg.loudness.ceiling_db = vf;
    mark_field(flags.loudness);
  } else if (key == "loudness.truePeakOversample") {
    cfg.loudness.true_peak_oversample = vi;
    mark_field(flags.loudness);
  } else if (key == "loudness.releaseMs") {
    cfg.loudness.release_ms = vf;
    mark_field(flags.loudness);
  } else if (key == "loudness.applyGainAtInputRate") {
    cfg.loudness.apply_gain_at_input_rate = v != 0.0;
    mark_field(flags.loudness);

  } else {
    throw std::invalid_argument("unknown chain config key: " + key);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// parse_chain_config_params
// ---------------------------------------------------------------------------

MasteringChainConfig parse_chain_config_params(const Param* params, std::size_t count) {
  MasteringChainConfig cfg;
  StageFlagsSet flags;

  for (std::size_t i = 0; i < count; ++i) {
    apply_one_param_to_config(cfg, params[i].key, params[i].value, flags);
  }

  cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  cfg.saturation.tape.enabled = resolve_enabled(flags.tape);
  cfg.saturation.exciter.enabled = resolve_enabled(flags.exciter);
  cfg.spectral.air_band.enabled = resolve_enabled(flags.air_band);
  cfg.stereo.imager.enabled = resolve_enabled(flags.imager);
  cfg.stereo.mono_maker.enabled = resolve_enabled(flags.mono_maker);
  cfg.maximizer.true_peak_limiter.enabled = resolve_enabled(flags.true_peak);
  cfg.loudness.enabled = resolve_enabled(flags.loudness);

  return cfg;
}

// ---------------------------------------------------------------------------
// apply_chain_config_overrides
// In-place differential update: dispatch each key through the shared helper,
// then for any module whose key was touched, update its `enabled` flag.
// Modules not mentioned in the overrides keep their existing `enabled` value.
// ---------------------------------------------------------------------------

void apply_chain_config_overrides(MasteringChainConfig& cfg, const Param* params,
                                  std::size_t count) {
  StageFlagsSet flags;

  for (std::size_t i = 0; i < count; ++i) {
    apply_one_param_to_config(cfg, params[i].key, params[i].value, flags);
  }

  if (flags.denoise.any_key_seen) {
    cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  }
  if (flags.tilt.any_key_seen) {
    cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  }
  if (flags.compressor.any_key_seen) {
    cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  }
  if (flags.tape.any_key_seen) {
    cfg.saturation.tape.enabled = resolve_enabled(flags.tape);
  }
  if (flags.exciter.any_key_seen) {
    cfg.saturation.exciter.enabled = resolve_enabled(flags.exciter);
  }
  if (flags.air_band.any_key_seen) {
    cfg.spectral.air_band.enabled = resolve_enabled(flags.air_band);
  }
  if (flags.imager.any_key_seen) {
    cfg.stereo.imager.enabled = resolve_enabled(flags.imager);
  }
  if (flags.mono_maker.any_key_seen) {
    cfg.stereo.mono_maker.enabled = resolve_enabled(flags.mono_maker);
  }
  if (flags.true_peak.any_key_seen) {
    cfg.maximizer.true_peak_limiter.enabled = resolve_enabled(flags.true_peak);
  }
  if (flags.loudness.any_key_seen) {
    cfg.loudness.enabled = resolve_enabled(flags.loudness);
  }
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

MonoChainResult run_chain_mono_params(const Param* params, std::size_t param_count,
                                      const float* samples, std::size_t length, int sample_rate) {
  MasteringChain chain(parse_chain_config_params(params, param_count));
  return chain.process_mono(samples, length, sample_rate);
}

StereoChainResult run_chain_stereo_params(const Param* params, std::size_t param_count,
                                          const float* left, const float* right, std::size_t length,
                                          int sample_rate) {
  MasteringChain chain(parse_chain_config_params(params, param_count));
  return chain.process_stereo(left, right, length, sample_rate);
}

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

struct StreamingMasteringChain::Impl {
  std::vector<std::unique_ptr<common::ProcessorBase>> processors;
};

StreamingMasteringChain::StreamingMasteringChain(MasteringChainConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
  if (config_.repair.denoise.enabled) {
    throw std::invalid_argument("StreamingMasteringChain does not support repair.denoise");
  }
  if (config_.loudness.enabled) {
    throw std::invalid_argument(
        "StreamingMasteringChain does not support loudness (whole-signal LUFS required)");
  }
}

StreamingMasteringChain::~StreamingMasteringChain() = default;
StreamingMasteringChain::StreamingMasteringChain(StreamingMasteringChain&&) noexcept = default;
StreamingMasteringChain& StreamingMasteringChain::operator=(StreamingMasteringChain&&) noexcept =
    default;

void StreamingMasteringChain::prepare(double sample_rate, int max_block_size, int num_channels) {
  if (num_channels != 1 && num_channels != 2) {
    throw std::invalid_argument("num_channels must be 1 or 2");
  }
  if (max_block_size <= 0) {
    throw std::invalid_argument("max_block_size must be > 0");
  }
  if (sample_rate <= 0.0) {
    throw std::invalid_argument("sample_rate must be > 0");
  }

  impl_->processors.clear();
  stage_names_.clear();

  auto add_stage = [&](std::unique_ptr<common::ProcessorBase> proc, const char* name) {
    proc->prepare(sample_rate, max_block_size);
    impl_->processors.push_back(std::move(proc));
    stage_names_.emplace_back(name);
  };

  // 1. eq.tilt
  if (config_.eq.tilt.enabled) {
    auto tilt = std::make_unique<mastering::eq::TiltEq>();
    tilt->set_tilt_db(config_.eq.tilt.tilt_db);
    tilt->set_pivot_hz(config_.eq.tilt.pivot_hz);
    add_stage(std::move(tilt), "eq.tilt");
  }

  // 2. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    add_stage(std::make_unique<mastering::dynamics::Compressor>(config_.dynamics.compressor.config),
              "dynamics.compressor");
  }

  // 3. saturation.tape
  if (config_.saturation.tape.enabled) {
    add_stage(std::make_unique<mastering::saturation::Tape>(config_.saturation.tape.config),
              "saturation.tape");
  }

  // 4. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    add_stage(std::make_unique<mastering::saturation::Exciter>(config_.saturation.exciter.config),
              "saturation.exciter");
  }

  // 5. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    add_stage(std::make_unique<mastering::spectral::AirBand>(config_.spectral.air_band.config),
              "spectral.airBand");
  }

  // 6. stereo.imager (stereo only)
  if (num_channels == 2 && config_.stereo.imager.enabled) {
    add_stage(std::make_unique<mastering::stereo::Imager>(config_.stereo.imager.config),
              "stereo.imager");
  }

  // 7. stereo.monoMaker (stereo only)
  if (num_channels == 2 && config_.stereo.mono_maker.enabled) {
    add_stage(std::make_unique<mastering::stereo::MonoMaker>(config_.stereo.mono_maker.config),
              "stereo.monoMaker");
  }

  // 8. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    add_stage(std::make_unique<mastering::maximizer::TruePeakLimiter>(
                  config_.maximizer.true_peak_limiter.config),
              "maximizer.truePeakLimiter");
  }

  prepared_channels_ = num_channels;
  max_block_size_ = max_block_size;
}

void StreamingMasteringChain::process_block(float* const* channels, int num_channels,
                                            int num_samples) {
  if (prepared_channels_ == 0) {
    throw std::logic_error("StreamingMasteringChain::process_block called before prepare()");
  }
  if (num_channels != prepared_channels_) {
    throw std::invalid_argument(
        "StreamingMasteringChain::process_block num_channels mismatch with prepare()");
  }
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw std::invalid_argument(
        "StreamingMasteringChain::process_block num_samples exceeds max_block_size from prepare()");
  }
  if (num_samples == 0) {
    return;
  }
  for (auto& proc : impl_->processors) {
    proc->process(channels, num_channels, num_samples);
  }
}

void StreamingMasteringChain::reset() {
  for (auto& proc : impl_->processors) {
    proc->reset();
  }
}

int StreamingMasteringChain::latency_samples() const noexcept {
  int total = 0;
  for (const auto& proc : impl_->processors) {
    total += proc->latency_samples();
  }
  return total;
}

}  // namespace sonare::mastering::api
