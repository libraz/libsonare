#pragma once

/// @file param_field_tables.h
/// @brief Single source of truth for mastering processor parameter fields.
///
/// Each processor's (jsonKey, config-member) pairs are listed exactly once, in
/// an X-macro table. The three consumers that previously repeated these lists
/// each expand the same table:
///   - processor_params.h  — builds a typed config from a flat ParamMap.
///   - chain_json.cpp       — serializes a config to JSON params.
///   - chain_params.cpp     — parses flat chain keys back into a config.
///
/// The tables carry no per-field type tag: the read (`assign`) and write
/// (`add_field`) helpers are overloaded on the config member's own type, so the
/// compiler — not a hand-maintained tag column — decides float/int/bool/enum
/// handling. Adding or renaming a parameter is a one-line table edit that all
/// three consumers pick up.

#include <cmath>
#include <type_traits>

namespace sonare::mastering::api::detail {

/// @brief Assigns a flat double param value to a typed config member.
/// @details One overload per storage kind so a table entry needs no type tag.
/// Integral and enum members round to the nearest integer (the flat param API
/// carries every value as a double); bool follows the "non-zero is true"
/// convention used throughout the chain param surface.
inline void assign_field(float& dst, double value) { dst = static_cast<float>(value); }
inline void assign_field(double& dst, double value) { dst = value; }
inline void assign_field(bool& dst, double value) { dst = value != 0.0; }

template <typename Int,
          std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>, int> = 0>
inline void assign_field(Int& dst, double value) {
  dst = static_cast<Int>(std::lround(value));
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
inline void assign_field(Enum& dst, double value) {
  dst = static_cast<Enum>(static_cast<int>(std::lround(value)));
}

}  // namespace sonare::mastering::api::detail

// ---------------------------------------------------------------------------
// Field tables. X(jsonKey, member) — jsonKey is the flat parameter name (the
// chain surface prefixes it with the stage path); member is the field on the
// processor's *Config struct. Order matches the historical serialization order
// so chain_config_to_json output is byte-stable.
// ---------------------------------------------------------------------------

// --- Dynamics ---

#define SONARE_FIELDS_COMPRESSOR(X)               \
  X("thresholdDb", threshold_db)                  \
  X("ratio", ratio)                               \
  X("attackMs", attack_ms)                        \
  X("releaseMs", release_ms)                      \
  X("kneeDb", knee_db)                            \
  X("makeupGainDb", makeup_gain_db)               \
  X("autoMakeup", auto_makeup)                    \
  X("detector", detector)                         \
  X("sidechainHpfEnabled", sidechain_hpf_enabled) \
  X("sidechainHpfHz", sidechain_hpf_hz)           \
  X("pdrTimeMs", pdr_time_ms)                     \
  X("pdrReleaseScale", pdr_release_scale)

#define SONARE_FIELDS_LIMITER(X) \
  X("thresholdDb", threshold_db) \
  X("lookaheadMs", lookahead_ms) \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_BRICKWALL_LIMITER(X) \
  X("ceilingDb", ceiling_db)               \
  X("lookaheadMs", lookahead_ms)           \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_DEESSER(X) \
  X("frequencyHz", frequency_hz) \
  X("thresholdDb", threshold_db) \
  X("ratio", ratio)              \
  X("attackMs", attack_ms)       \
  X("releaseMs", release_ms)     \
  X("rangeDb", range_db)         \
  X("bandpassQ", bandpass_q)

#define SONARE_FIELDS_EXPANDER(X) \
  X("thresholdDb", threshold_db)  \
  X("ratio", ratio)               \
  X("attackMs", attack_ms)        \
  X("releaseMs", release_ms)      \
  X("rangeDb", range_db)

#define SONARE_FIELDS_GATE(X)               \
  X("thresholdDb", threshold_db)            \
  X("attackMs", attack_ms)                  \
  X("releaseMs", release_ms)                \
  X("rangeDb", range_db)                    \
  X("holdMs", hold_ms)                      \
  X("closeThresholdDb", close_threshold_db) \
  X("keyHpfHz", key_hpf_hz)

#define SONARE_FIELDS_PARALLEL_COMP(X)   \
  X("thresholdDb", threshold_db)         \
  X("ratio", ratio)                      \
  X("attackMs", attack_ms)               \
  X("releaseMs", release_ms)             \
  X("makeupGainDb", makeup_gain_db)      \
  X("mix", mix)                          \
  X("linkedDetection", linked_detection) \
  X("outputLimiter", output_limiter)     \
  X("outputCeilingDb", output_ceiling_db)

#define SONARE_FIELDS_SIDECHAIN_ROUTER(X)         \
  X("thresholdDb", threshold_db)                  \
  X("ratio", ratio)                               \
  X("attackMs", attack_ms)                        \
  X("releaseMs", release_ms)                      \
  X("rangeDb", range_db)                          \
  X("sidechainHpfEnabled", sidechain_hpf_enabled) \
  X("sidechainHpfHz", sidechain_hpf_hz)           \
  X("monoSumming", mono_summing)                  \
  X("keyListen", key_listen)

#define SONARE_FIELDS_DUCKING(X) \
  X("thresholdDb", threshold_db) \
  X("ratio", ratio)              \
  X("attackMs", attack_ms)       \
  X("releaseMs", release_ms)     \
  X("rangeDb", range_db)         \
  X("lookaheadMs", lookahead_ms)

#define SONARE_FIELDS_TRANSIENT_SHAPER(X) \
  X("attackGainDb", attack_gain_db)       \
  X("sustainGainDb", sustain_gain_db)     \
  X("fastAttackMs", fast_attack_ms)       \
  X("fastReleaseMs", fast_release_ms)     \
  X("slowAttackMs", slow_attack_ms)       \
  X("slowReleaseMs", slow_release_ms)     \
  X("sensitivity", sensitivity)           \
  X("maxGainDb", max_gain_db)             \
  X("gainSmoothingMs", gain_smoothing_ms) \
  X("lookaheadMs", lookahead_ms)

#define SONARE_FIELDS_UPWARD_COMPRESSOR(X) \
  X("thresholdDb", threshold_db)           \
  X("ratio", ratio)                        \
  X("attackMs", attack_ms)                 \
  X("releaseMs", release_ms)               \
  X("rangeDb", range_db)

#define SONARE_FIELDS_UPWARD_EXPANDER(X) SONARE_FIELDS_UPWARD_COMPRESSOR(X)

#define SONARE_FIELDS_VOCAL_RIDER(X)      \
  X("targetDb", target_db)                \
  X("maxBoostDb", max_boost_db)           \
  X("maxCutDb", max_cut_db)               \
  X("attackMs", attack_ms)                \
  X("releaseMs", release_ms)              \
  X("outputGainDb", output_gain_db)       \
  X("gainSmoothingMs", gain_smoothing_ms) \
  X("noiseFloorDb", noise_floor_db)       \
  X("linkedDetection", linked_detection)

// --- Saturation ---

#define SONARE_FIELDS_TAPE(X)       \
  X("driveDb", drive_db)            \
  X("saturation", saturation)       \
  X("hysteresis", hysteresis)       \
  X("outputGainDb", output_gain_db) \
  X("speedIps", speed_ips)          \
  X("headBumpDb", head_bump_db)     \
  X("bias", bias)                   \
  X("gapLoss", gap_loss)

#define SONARE_FIELDS_EXCITER(X) \
  X("frequencyHz", frequency_hz) \
  X("driveDb", drive_db)         \
  X("amount", amount)            \
  X("q", q)                      \
  X("evenOddMix", even_odd_mix)

#define SONARE_FIELDS_BITCRUSHER(X)        \
  X("bitDepth", bit_depth)                 \
  X("downsampleFactor", downsample_factor) \
  X("mix", mix)                            \
  X("ditherType", dither_type)             \
  X("ditherSeed", dither_seed)

#define SONARE_FIELDS_HARD_CLIPPER(X) X("ceiling", ceiling)

#define SONARE_FIELDS_SOFT_CLIPPER(X) \
  X("driveDb", drive_db)              \
  X("ceiling", ceiling)               \
  X("mix", mix)

#define SONARE_FIELDS_WAVESHAPER(X) \
  X("driveDb", drive_db)            \
  X("mix", mix)                     \
  X("outputGainDb", output_gain_db) \
  X("bias", bias)                   \
  X("curve", curve)

#define SONARE_FIELDS_TUBE(X)              \
  X("driveDb", drive_db)                   \
  X("bias", bias)                          \
  X("mix", mix)                            \
  X("oversampleFactor", oversample_factor) \
  X("biasV", bias_v)                       \
  X("harmonicDrive", harmonic_drive)

#define SONARE_FIELDS_TRANSFORMER(X) \
  X("driveDb", drive_db)             \
  X("asymmetry", asymmetry)          \
  X("mix", mix)

// --- Spectral ---

#define SONARE_FIELDS_AIR_BAND(X)               \
  X("amount", amount)                           \
  X("shelfFrequencyHz", shelf_frequency_hz)     \
  X("dynamicThresholdDb", dynamic_threshold_db) \
  X("dynamicRangeDb", dynamic_range_db)

#define SONARE_FIELDS_LOW_END_FOCUS(X)       \
  X("cutoffHz", cutoff_hz)                   \
  X("width", width)                          \
  X("subharmonicAmount", subharmonic_amount) \
  X("transientTightness", transient_tightness)

#define SONARE_FIELDS_PRESENCE_ENHANCER(X)    \
  X("amount", amount)                         \
  X("drive", drive)                           \
  X("centerFrequencyHz", center_frequency_hz) \
  X("q", q)

#define SONARE_FIELDS_SPECTRAL_SHAPER(X)  \
  X("threshold", threshold)               \
  X("amount", amount)                     \
  X("frequencyHz", frequency_hz)          \
  X("highFrequencyHz", high_frequency_hz) \
  X("attackMs", attack_ms)                \
  X("releaseMs", release_ms)              \
  X("rangeDb", range_db)

// --- Stereo ---

#define SONARE_FIELDS_AUTO_PAN(X) \
  X("rateHz", rate_hz)            \
  X("depth", depth)               \
  X("phase", phase)

#define SONARE_FIELDS_HAAS_ENHANCER(X) \
  X("delayMs", delay_ms)               \
  X("mix", mix)                        \
  X("delayRight", delay_right)

// The imager additionally range-checks width/decorrelationAmount in its config
// builder; only the field overlay is table-driven.
#define SONARE_FIELDS_IMAGER(X)                  \
  X("width", width)                              \
  X("outputGainDb", output_gain_db)              \
  X("decorrelationAmount", decorrelation_amount) \
  X("preserveEnergy", preserve_energy)

#define SONARE_FIELDS_MONO_MAKER(X) X("amount", amount)

#define SONARE_FIELDS_PHASE_ALIGN(X) \
  X("delaySamples", delay_samples)   \
  X("delayRight", delay_right)       \
  X("fractionalDelaySamples", fractional_delay_samples)

#define SONARE_FIELDS_STEREO_BALANCE(X) \
  X("balance", balance)                 \
  X("constantPower", constant_power)

// --- Maximizer ---

#define SONARE_FIELDS_MAXIMIZER(X) \
  X("inputGainDb", input_gain_db)  \
  X("ceilingDb", ceiling_db)       \
  X("lookaheadMs", lookahead_ms)   \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_TRUE_PEAK_LIMITER(X) \
  X("ceilingDb", ceiling_db)               \
  X("lookaheadMs", lookahead_ms)           \
  X("releaseMs", release_ms)               \
  X("oversampleFactor", oversample_factor) \
  X("applyGainAtInputRate", apply_gain_at_input_rate)

#define SONARE_FIELDS_SOFT_KNEE_MAX(X) \
  X("inputGainDb", input_gain_db)      \
  X("ceilingDb", ceiling_db)           \
  X("kneeDb", knee_db)                 \
  X("releaseMs", release_ms)

#define SONARE_FIELDS_ADAPTIVE_RELEASE(X) \
  X("ceilingDb", ceiling_db)              \
  X("lookaheadMs", lookahead_ms)          \
  X("minReleaseMs", min_release_ms)       \
  X("maxReleaseMs", max_release_ms)       \
  X("crestWindowMs", crest_window_ms)     \
  X("crestLow", crest_low)                \
  X("crestHigh", crest_high)              \
  X("releaseSmoothingMs", release_smoothing_ms)
