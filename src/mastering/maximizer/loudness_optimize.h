#pragma once

/// @file loudness_optimize.h
/// @brief Offline loudness normalization helper with peak ceiling.

#include "core/audio.h"

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
};

struct LoudnessOptimizeResult {
  Audio audio;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  /// Static gain applied before the true-peak limiter. This deliberately does
  /// not include limiter gain reduction.
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

/// @brief Single-pass loudness normalization followed by a true-peak limiter.
/// @details The helper computes one static gain from input LUFS and true peak,
/// then limits residual overs. It does not iterate after limiting, so clipped or
/// very peaky material may finish below target LUFS while respecting the
/// ceiling.
LoudnessOptimizeResult loudness_optimize(const Audio& audio,
                                         const LoudnessOptimizeConfig& config = {});

}  // namespace sonare::mastering::maximizer
