/// @file chain_params.cpp
/// @brief Flat-parameter bridge for the high-level mastering chain.

#include <stdexcept>
#include <string>

#include "mastering/api/chain.h"

namespace sonare::mastering::api {
namespace {

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
// ---------------------------------------------------------------------------
// Per-key dispatch shared by parse_chain_config_params and
// apply_chain_config_overrides.
// ---------------------------------------------------------------------------

struct StageFlagsSet {
  StageFlags declick;
  StageFlags dereverb;
  StageFlags denoise;
  StageFlags tilt;
  StageFlags deesser;
  StageFlags transient_shaper;
  StageFlags compressor;
  StageFlags multiband_comp;
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

  // ---- repair.declick ----
  if (key == "repair.declick.enabled") {
    mark_enabled(flags.declick, v);
  } else if (key == "repair.declick.threshold") {
    cfg.repair.declick.config.threshold = vf;
    mark_field(flags.declick);
  } else if (key == "repair.declick.neighborRatio") {
    cfg.repair.declick.config.neighbor_ratio = vf;
    mark_field(flags.declick);
  } else if (key == "repair.declick.maxClickSamples") {
    cfg.repair.declick.config.max_click_samples = static_cast<size_t>(vi);
    mark_field(flags.declick);
  } else if (key == "repair.declick.lpcOrder") {
    cfg.repair.declick.config.lpc_order = vi;
    mark_field(flags.declick);
  } else if (key == "repair.declick.residualRatio") {
    cfg.repair.declick.config.residual_ratio = vf;
    mark_field(flags.declick);

    // ---- repair.dereverb ----
  } else if (key == "repair.dereverb.enabled") {
    mark_enabled(flags.dereverb, v);
  } else if (key == "repair.dereverb.threshold") {
    cfg.repair.dereverb.config.threshold = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.attenuation") {
    cfg.repair.dereverb.config.attenuation = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.nFft") {
    cfg.repair.dereverb.config.n_fft = vi;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.hopLength") {
    cfg.repair.dereverb.config.hop_length = vi;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.t60Sec") {
    cfg.repair.dereverb.config.t60_sec = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.lateDelayMs") {
    cfg.repair.dereverb.config.late_delay_ms = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.overSubtraction") {
    cfg.repair.dereverb.config.over_subtraction = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.spectralFloor") {
    cfg.repair.dereverb.config.spectral_floor = vf;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.wpeEnabled") {
    cfg.repair.dereverb.config.wpe_enabled = v != 0.0;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.wpeIterations") {
    cfg.repair.dereverb.config.wpe_iterations = vi;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.wpeTaps") {
    cfg.repair.dereverb.config.wpe_taps = vi;
    mark_field(flags.dereverb);
  } else if (key == "repair.dereverb.wpeStrength") {
    cfg.repair.dereverb.config.wpe_strength = vf;
    mark_field(flags.dereverb);

    // ---- repair.denoise ----
  } else if (key == "repair.denoise.enabled") {
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

    // ---- dynamics.deesser ----
  } else if (key == "dynamics.deesser.enabled") {
    mark_enabled(flags.deesser, v);
  } else if (key == "dynamics.deesser.frequencyHz") {
    cfg.dynamics.deesser.config.frequency_hz = vf;
    mark_field(flags.deesser);
  } else if (key == "dynamics.deesser.thresholdDb") {
    cfg.dynamics.deesser.config.threshold_db = vf;
    mark_field(flags.deesser);
  } else if (key == "dynamics.deesser.ratio") {
    cfg.dynamics.deesser.config.ratio = vf;
    mark_field(flags.deesser);
  } else if (key == "dynamics.deesser.attackMs") {
    cfg.dynamics.deesser.config.attack_ms = vf;
    mark_field(flags.deesser);
  } else if (key == "dynamics.deesser.releaseMs") {
    cfg.dynamics.deesser.config.release_ms = vf;
    mark_field(flags.deesser);
  } else if (key == "dynamics.deesser.rangeDb") {
    cfg.dynamics.deesser.config.range_db = vf;
    mark_field(flags.deesser);

    // ---- dynamics.transientShaper ----
  } else if (key == "dynamics.transientShaper.enabled") {
    mark_enabled(flags.transient_shaper, v);
  } else if (key == "dynamics.transientShaper.attackGainDb") {
    cfg.dynamics.transient_shaper.config.attack_gain_db = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.sustainGainDb") {
    cfg.dynamics.transient_shaper.config.sustain_gain_db = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.fastAttackMs") {
    cfg.dynamics.transient_shaper.config.fast_attack_ms = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.fastReleaseMs") {
    cfg.dynamics.transient_shaper.config.fast_release_ms = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.slowAttackMs") {
    cfg.dynamics.transient_shaper.config.slow_attack_ms = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.slowReleaseMs") {
    cfg.dynamics.transient_shaper.config.slow_release_ms = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.sensitivity") {
    cfg.dynamics.transient_shaper.config.sensitivity = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.maxGainDb") {
    cfg.dynamics.transient_shaper.config.max_gain_db = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.gainSmoothingMs") {
    cfg.dynamics.transient_shaper.config.gain_smoothing_ms = vf;
    mark_field(flags.transient_shaper);
  } else if (key == "dynamics.transientShaper.lookaheadMs") {
    cfg.dynamics.transient_shaper.config.lookahead_ms = vf;
    mark_field(flags.transient_shaper);

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

    // ---- dynamics.multibandComp ----
  } else if (key == "dynamics.multibandComp.enabled") {
    mark_enabled(flags.multiband_comp, v);
  } else if (key == "dynamics.multibandComp.lowCutoffHz") {
    if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 1) {
      cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[0] = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.highCutoffHz") {
    if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 2) {
      cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[1] = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.lowThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.lowRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].ratio = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.lowAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.lowReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].release_ms = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.midThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.midRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].ratio = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.midAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.midReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].release_ms = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.highThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.highRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].ratio = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.highAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
  } else if (key == "dynamics.multibandComp.highReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].release_ms = vf;
    }
    mark_field(flags.multiband_comp);

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

  cfg.repair.declick.enabled = resolve_enabled(flags.declick);
  cfg.repair.dereverb.enabled = resolve_enabled(flags.dereverb);
  cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  cfg.dynamics.deesser.enabled = resolve_enabled(flags.deesser);
  cfg.dynamics.transient_shaper.enabled = resolve_enabled(flags.transient_shaper);
  cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  cfg.dynamics.multiband_comp.enabled = resolve_enabled(flags.multiband_comp);
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

  if (flags.declick.any_key_seen) {
    cfg.repair.declick.enabled = resolve_enabled(flags.declick);
  }
  if (flags.dereverb.any_key_seen) {
    cfg.repair.dereverb.enabled = resolve_enabled(flags.dereverb);
  }
  if (flags.denoise.any_key_seen) {
    cfg.repair.denoise.enabled = resolve_enabled(flags.denoise);
  }
  if (flags.tilt.any_key_seen) {
    cfg.eq.tilt.enabled = resolve_enabled(flags.tilt);
  }
  if (flags.deesser.any_key_seen) {
    cfg.dynamics.deesser.enabled = resolve_enabled(flags.deesser);
  }
  if (flags.transient_shaper.any_key_seen) {
    cfg.dynamics.transient_shaper.enabled = resolve_enabled(flags.transient_shaper);
  }
  if (flags.compressor.any_key_seen) {
    cfg.dynamics.compressor.enabled = resolve_enabled(flags.compressor);
  }
  if (flags.multiband_comp.any_key_seen) {
    cfg.dynamics.multiband_comp.enabled = resolve_enabled(flags.multiband_comp);
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

}  // namespace sonare::mastering::api
