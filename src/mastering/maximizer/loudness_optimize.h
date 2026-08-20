#pragma once

/// @file loudness_optimize.h
/// @brief Offline loudness normalization helper with peak ceiling.

#include "core/audio.h"
#include "mastering/maximizer/true_peak_limiter.h"

namespace sonare::mastering::maximizer {

struct LoudnessOptimizeConfig {
  float target_lufs = -14.0f;
  float ceiling_db = -1.0f;
  int true_peak_oversample = 4;
  /// Release time of the post true-peak limiter, in milliseconds. Mirrors
  /// @ref TruePeakLimiterConfig::release_ms so the standalone helper and the
  /// in-chain loudness stage limit identically.
  float release_ms = 50.0f;
  /// @copydoc TruePeakLimiterConfig::apply_gain_at_input_rate
  bool apply_gain_at_input_rate = false;
  /// @brief How deep (dB, >= 0) the helper may drive its post-gain true-peak
  ///        limiter to reach @ref target_lufs.
  /// @details The static gain may exceed the peak headroom toward
  ///          @ref ceiling_db by this much; 0 restores a strict headroom clamp,
  ///          under which peak-normalized input keeps its input loudness
  ///          whatever target is asked for. Mirrors
  ///          @ref sonare::mastering::api::LoudnessStage::max_limiter_gain_reduction_db.
  float max_limiter_gain_reduction_db = kDefaultLoudnessMaxLimiterGainReductionDb;
};

struct LoudnessOptimizeResult {
  Audio audio;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  /// Static gain applied before the true-peak limiter. This deliberately does
  /// not include limiter gain reduction.
  float applied_gain_db = 0.0f;
  /// True when the helper did not deliver @ref LoudnessOptimizeConfig::target_lufs:
  /// either peak headroom clamped the requested normalization gain, or the
  /// true-peak limiter pulled the achieved loudness back below the target.
  bool loudness_target_limited = false;
  /// Always 0: the returned audio is time-aligned because this helper
  /// compensates the internal true-peak limiter's look-ahead latency itself.
  int latency_samples = 0;
};

/// @brief Single-pass loudness normalization followed by a true-peak limiter.
/// @details The helper computes one static gain from input LUFS and true peak,
/// then limits the resulting overs. It does not iterate after limiting, so
/// material needing more than
/// @ref LoudnessOptimizeConfig::max_limiter_gain_reduction_db of limiting may
/// finish below target LUFS while respecting the ceiling. Inspect
/// LoudnessOptimizeResult::loudness_target_limited to distinguish that outcome
/// from a reached target.
LoudnessOptimizeResult loudness_optimize(const Audio& audio,
                                         const LoudnessOptimizeConfig& config = {});

}  // namespace sonare::mastering::maximizer
