/// @file chain_params.cpp
/// @brief Flat-parameter bridge for the high-level mastering chain.

#include <string>

#include "mastering/api/chain.h"
#include "mastering/api/param_field_tables.h"
#include "util/exception.h"

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
  StageFlags declip;
  StageFlags decrackle;
  StageFlags dehum;
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

// Each per-stage helper handles one cluster of keys and returns true if the key
// was recognized (and applied). A flat sequence of independent early-return
// `if` blocks keeps the block-nesting depth at 1, which avoids MSVC error
// C1061 ("blocks nested too deeply") that a long else-if chain triggers.

// ---- repair.* (declick, declip, decrackle, dehum, dereverb, denoise) ----
bool apply_repair_param(MasteringChainConfig& cfg, const std::string& key, double v,
                        StageFlagsSet& flags) {
  // Repair stages keep bespoke parsing (size_t / enum-clamping quirks), so they
  // still need the float / int views of the value.
  const float vf = static_cast<float>(v);
  const int vi = static_cast<int>(v);
  // ---- repair.declick ----
  if (key == "repair.declick.enabled") {
    mark_enabled(flags.declick, v);
    return true;
  }
  if (key == "repair.declick.threshold") {
    cfg.repair.declick.config.threshold = vf;
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.neighborRatio") {
    cfg.repair.declick.config.neighbor_ratio = vf;
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.maxClickSamples") {
    cfg.repair.declick.config.max_click_samples = static_cast<size_t>(vi);
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.lpcOrder") {
    cfg.repair.declick.config.lpc_order = vi;
    mark_field(flags.declick);
    return true;
  }
  if (key == "repair.declick.residualRatio") {
    cfg.repair.declick.config.residual_ratio = vf;
    mark_field(flags.declick);
    return true;
  }

  // ---- repair.declip ----
  if (key == "repair.declip.enabled") {
    mark_enabled(flags.declip, v);
    return true;
  }
  if (key == "repair.declip.clipThreshold") {
    cfg.repair.declip.config.clip_threshold = vf;
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.lpcOrder") {
    cfg.repair.declip.config.lpc_order = vi;
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.iterations") {
    cfg.repair.declip.config.iterations = vi;
    mark_field(flags.declip);
    return true;
  }
  if (key == "repair.declip.lpcBlend") {
    cfg.repair.declip.config.lpc_blend = vf;
    mark_field(flags.declip);
    return true;
  }

  // ---- repair.decrackle ----
  if (key == "repair.decrackle.enabled") {
    mark_enabled(flags.decrackle, v);
    return true;
  }
  if (key == "repair.decrackle.threshold") {
    cfg.repair.decrackle.config.threshold = vf;
    mark_field(flags.decrackle);
    return true;
  }
  if (key == "repair.decrackle.mode") {
    cfg.repair.decrackle.config.mode = vi == 1 ? mastering::repair::DecrackleMode::WaveletShrinkage
                                               : mastering::repair::DecrackleMode::Median;
    mark_field(flags.decrackle);
    return true;
  }
  if (key == "repair.decrackle.levels") {
    cfg.repair.decrackle.config.levels = vi;
    mark_field(flags.decrackle);
    return true;
  }

  // ---- repair.dehum ----
  if (key == "repair.dehum.enabled") {
    mark_enabled(flags.dehum, v);
    return true;
  }
  if (key == "repair.dehum.fundamentalHz") {
    cfg.repair.dehum.config.fundamental_hz = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.harmonics") {
    cfg.repair.dehum.config.harmonics = vi;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.q") {
    cfg.repair.dehum.config.q = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.adaptive") {
    cfg.repair.dehum.config.adaptive = v != 0.0;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.searchRangeHz") {
    cfg.repair.dehum.config.search_range_hz = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.adaptation") {
    cfg.repair.dehum.config.adaptation = vf;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.frameSize") {
    cfg.repair.dehum.config.frame_size = vi;
    mark_field(flags.dehum);
    return true;
  }
  if (key == "repair.dehum.pllBandwidth") {
    cfg.repair.dehum.config.pll_bandwidth = vf;
    mark_field(flags.dehum);
    return true;
  }

  // ---- repair.dereverb ----
  if (key == "repair.dereverb.enabled") {
    mark_enabled(flags.dereverb, v);
    return true;
  }
  if (key == "repair.dereverb.threshold") {
    cfg.repair.dereverb.config.threshold = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.attenuation") {
    cfg.repair.dereverb.config.attenuation = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.nFft") {
    cfg.repair.dereverb.config.n_fft = vi;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.hopLength") {
    cfg.repair.dereverb.config.hop_length = vi;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.t60Sec") {
    cfg.repair.dereverb.config.t60_sec = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.lateDelayMs") {
    cfg.repair.dereverb.config.late_delay_ms = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.overSubtraction") {
    cfg.repair.dereverb.config.over_subtraction = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.spectralFloor") {
    cfg.repair.dereverb.config.spectral_floor = vf;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeEnabled") {
    cfg.repair.dereverb.config.wpe_enabled = v != 0.0;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeIterations") {
    cfg.repair.dereverb.config.wpe_iterations = vi;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeTaps") {
    cfg.repair.dereverb.config.wpe_taps = vi;
    mark_field(flags.dereverb);
    return true;
  }
  if (key == "repair.dereverb.wpeStrength") {
    cfg.repair.dereverb.config.wpe_strength = vf;
    mark_field(flags.dereverb);
    return true;
  }

  // ---- repair.denoise ----
  if (key == "repair.denoise.enabled") {
    mark_enabled(flags.denoise, v);
    return true;
  }
  if (key == "repair.denoise.mode") {
    cfg.repair.denoise.config.mode = static_cast<repair::DenoiseMode>(vi);
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.noiseEstimator") {
    cfg.repair.denoise.config.noise_estimator = static_cast<repair::DenoiseNoiseEstimator>(vi);
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.nFft") {
    cfg.repair.denoise.config.n_fft = vi;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.hopLength") {
    cfg.repair.denoise.config.hop_length = vi;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.ddAlpha") {
    cfg.repair.denoise.config.dd_alpha = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.gainFloor") {
    cfg.repair.denoise.config.gain_floor = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.overSubtraction") {
    cfg.repair.denoise.config.over_subtraction = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.spectralFloor") {
    cfg.repair.denoise.config.spectral_floor = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.noiseEstimationQuantile") {
    cfg.repair.denoise.config.noise_estimation_quantile = vf;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.speechPresenceGain") {
    cfg.repair.denoise.config.speech_presence_gain = v != 0.0;
    mark_field(flags.denoise);
    return true;
  }
  if (key == "repair.denoise.gainSmoothing") {
    cfg.repair.denoise.config.gain_smoothing = v != 0.0;
    mark_field(flags.denoise);
    return true;
  }

  return false;
}

// ---- eq.tilt + dynamics.* (deesser, transientShaper, compressor, multibandComp) ----
bool apply_eq_dynamics_param(MasteringChainConfig& cfg, const std::string& key, double v,
                             StageFlagsSet& flags) {
  // ---- eq.tilt ----
  if (key == "eq.tilt.enabled") {
    mark_enabled(flags.tilt, v);
    return true;
  }
#define X(jkey, member)                          \
  if (key == "eq.tilt." jkey) {                  \
    detail::assign_field(cfg.eq.tilt.member, v); \
    mark_field(flags.tilt);                      \
    return true;                                 \
  }
  SONARE_FIELDS_EQ_TILT(X)
#undef X

  // ---- dynamics.deesser ----
  if (key == "dynamics.deesser.enabled") {
    mark_enabled(flags.deesser, v);
    return true;
  }
#define X(jkey, member)                                          \
  if (key == "dynamics.deesser." jkey) {                         \
    detail::assign_field(cfg.dynamics.deesser.config.member, v); \
    mark_field(flags.deesser);                                   \
    return true;                                                 \
  }
  SONARE_FIELDS_DEESSER(X)
#undef X

  // ---- dynamics.transientShaper ----
  if (key == "dynamics.transientShaper.enabled") {
    mark_enabled(flags.transient_shaper, v);
    return true;
  }
#define X(jkey, member)                                                   \
  if (key == "dynamics.transientShaper." jkey) {                          \
    detail::assign_field(cfg.dynamics.transient_shaper.config.member, v); \
    mark_field(flags.transient_shaper);                                   \
    return true;                                                          \
  }
  SONARE_FIELDS_TRANSIENT_SHAPER(X)
#undef X

  // ---- dynamics.compressor ----
  // `detector` is an enum restored via the enum assign_field overload, matching
  // the integer it was serialized as.
  if (key == "dynamics.compressor.enabled") {
    mark_enabled(flags.compressor, v);
    return true;
  }
#define X(jkey, member)                                             \
  if (key == "dynamics.compressor." jkey) {                         \
    detail::assign_field(cfg.dynamics.compressor.config.member, v); \
    mark_field(flags.compressor);                                   \
    return true;                                                    \
  }
  SONARE_FIELDS_COMPRESSOR(X)
#undef X

  // ---- dynamics.multibandComp ----
  // Bespoke: per-band fields write into bounds-checked crossover/bands vectors.
  const float vf = static_cast<float>(v);
  if (key == "dynamics.multibandComp.enabled") {
    mark_enabled(flags.multiband_comp, v);
    return true;
  }
  if (key == "dynamics.multibandComp.lowCutoffHz") {
    if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 1) {
      cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[0] = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.highCutoffHz") {
    if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 2) {
      cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[1] = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.lowThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.lowRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].ratio = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.lowAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.lowReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 1) {
      cfg.dynamics.multiband_comp.config.bands[0].release_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.midThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.midRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].ratio = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.midAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.midReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 2) {
      cfg.dynamics.multiband_comp.config.bands[1].release_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.highThresholdDb") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].threshold_db = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.highRatio") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].ratio = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.highAttackMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].attack_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }
  if (key == "dynamics.multibandComp.highReleaseMs") {
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      cfg.dynamics.multiband_comp.config.bands[2].release_ms = vf;
    }
    mark_field(flags.multiband_comp);
    return true;
  }

  return false;
}

// ---- saturation.* (tape, exciter) ----
bool apply_saturation_param(MasteringChainConfig& cfg, const std::string& key, double v,
                            StageFlagsSet& flags) {
  // ---- saturation.tape ----
  if (key == "saturation.tape.enabled") {
    mark_enabled(flags.tape, v);
    return true;
  }
#define X(jkey, member)                                         \
  if (key == "saturation.tape." jkey) {                         \
    detail::assign_field(cfg.saturation.tape.config.member, v); \
    mark_field(flags.tape);                                     \
    return true;                                                \
  }
  SONARE_FIELDS_TAPE(X)
#undef X

  // ---- saturation.exciter ----
  if (key == "saturation.exciter.enabled") {
    mark_enabled(flags.exciter, v);
    return true;
  }
#define X(jkey, member)                                            \
  if (key == "saturation.exciter." jkey) {                         \
    detail::assign_field(cfg.saturation.exciter.config.member, v); \
    mark_field(flags.exciter);                                     \
    return true;                                                   \
  }
  SONARE_FIELDS_EXCITER(X)
#undef X

  return false;
}

// ---- spectral.airBand + stereo.* (imager, monoMaker) ----
bool apply_spectral_stereo_param(MasteringChainConfig& cfg, const std::string& key, double v,
                                 StageFlagsSet& flags) {
  // ---- spectral.airBand ----
  if (key == "spectral.airBand.enabled") {
    mark_enabled(flags.air_band, v);
    return true;
  }
#define X(jkey, member)                                           \
  if (key == "spectral.airBand." jkey) {                          \
    detail::assign_field(cfg.spectral.air_band.config.member, v); \
    mark_field(flags.air_band);                                   \
    return true;                                                  \
  }
  SONARE_FIELDS_AIR_BAND(X)
#undef X

  // ---- stereo.imager ----
  if (key == "stereo.imager.enabled") {
    mark_enabled(flags.imager, v);
    return true;
  }
#define X(jkey, member)                                       \
  if (key == "stereo.imager." jkey) {                         \
    detail::assign_field(cfg.stereo.imager.config.member, v); \
    mark_field(flags.imager);                                 \
    return true;                                              \
  }
  SONARE_FIELDS_IMAGER(X)
#undef X

  // ---- stereo.monoMaker ----
  if (key == "stereo.monoMaker.enabled") {
    mark_enabled(flags.mono_maker, v);
    return true;
  }
#define X(jkey, member)                                           \
  if (key == "stereo.monoMaker." jkey) {                          \
    detail::assign_field(cfg.stereo.mono_maker.config.member, v); \
    mark_field(flags.mono_maker);                                 \
    return true;                                                  \
  }
  SONARE_FIELDS_MONO_MAKER(X)
#undef X

  return false;
}

// ---- maximizer.truePeakLimiter + loudness ----
bool apply_maximizer_loudness_param(MasteringChainConfig& cfg, const std::string& key, double v,
                                    StageFlagsSet& flags) {
  // ---- maximizer.truePeakLimiter ----
  if (key == "maximizer.truePeakLimiter.enabled") {
    mark_enabled(flags.true_peak, v);
    return true;
  }
#define X(jkey, member)                                                     \
  if (key == "maximizer.truePeakLimiter." jkey) {                           \
    detail::assign_field(cfg.maximizer.true_peak_limiter.config.member, v); \
    mark_field(flags.true_peak);                                            \
    return true;                                                            \
  }
  SONARE_FIELDS_TRUE_PEAK_LIMITER(X)
#undef X

  // ---- loudness ----
  if (key == "loudness.enabled") {
    mark_enabled(flags.loudness, v);
    return true;
  }
#define X(jkey, member)                           \
  if (key == "loudness." jkey) {                  \
    detail::assign_field(cfg.loudness.member, v); \
    mark_field(flags.loudness);                   \
    return true;                                  \
  }
  SONARE_FIELDS_LOUDNESS(X)
#undef X

  return false;
}

void apply_one_param_to_config(MasteringChainConfig& cfg, const std::string& key, double v,
                               StageFlagsSet& flags) {
  if (apply_repair_param(cfg, key, v, flags)) return;
  if (apply_eq_dynamics_param(cfg, key, v, flags)) return;
  if (apply_saturation_param(cfg, key, v, flags)) return;
  if (apply_spectral_stereo_param(cfg, key, v, flags)) return;
  if (apply_maximizer_loudness_param(cfg, key, v, flags)) return;
  throw SonareException(ErrorCode::InvalidParameter, "unknown chain config key: " + key);
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
  cfg.repair.declip.enabled = resolve_enabled(flags.declip);
  cfg.repair.decrackle.enabled = resolve_enabled(flags.decrackle);
  cfg.repair.dehum.enabled = resolve_enabled(flags.dehum);
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
  if (flags.declip.any_key_seen) {
    cfg.repair.declip.enabled = resolve_enabled(flags.declip);
  }
  if (flags.decrackle.any_key_seen) {
    cfg.repair.decrackle.enabled = resolve_enabled(flags.decrackle);
  }
  if (flags.dehum.any_key_seen) {
    cfg.repair.dehum.enabled = resolve_enabled(flags.dehum);
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
