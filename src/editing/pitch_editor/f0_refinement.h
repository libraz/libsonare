#pragma once

/// @file f0_refinement.h
/// @brief Refines a tracked F0 locally against the source waveform.

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare::editing::pitch_editor::detail {

// One quarter semitone is wide enough to catch a pYIN grid-centre estimate and
// narrow enough to keep refinement on the Viterbi-selected period.
constexpr double kRefineWindowCents = 25.0;

/// @brief Refines one frame's F0 by minimizing a fixed-window YIN difference.
/// @details The decoded candidate chooses the period neighbourhood, and the
///          waveform supplies its continuous value. A fixed sample window
///          keeps the difference comparable across integer lags; a parabolic
///          minimum supplies the fractional lag. The operation follows the
///          difference function in YIN (de Cheveigné and Kawahara, 2002,
///          https://doi.org/10.1121/1.1458024).
///
///          If the lag minimum falls on the search edge or is not convex, the
///          input estimate is retained. Scratch storage is reused by callers.
inline float refine_frame_f0(const Audio& source, int64_t frame_begin, int frame_length,
                             float f0_hz, std::vector<float>& frame_samples,
                             std::vector<double>& difference) {
  const double sample_rate = source.sample_rate();
  const double period = sample_rate / static_cast<double>(f0_hz);
  const double ratio = std::pow(2.0, kRefineWindowCents / constants::kCentsPerOctave);
  const double lag_low_raw = std::floor(period / ratio);
  const double lag_high_raw = std::ceil(period * ratio);
  if (!(lag_low_raw >= 1.0) || !(lag_high_raw + 1.0 < static_cast<double>(frame_length))) {
    return f0_hz;
  }
  const int lag_low = static_cast<int>(lag_low_raw);
  const int lag_high = static_cast<int>(lag_high_raw);
  const int window = frame_length - lag_high;

  frame_samples.assign(static_cast<size_t>(frame_length), 0.0f);
  const int64_t source_size = static_cast<int64_t>(source.size());
  for (int n = 0; n < frame_length; ++n) {
    const int64_t sample = frame_begin + n;
    if (sample >= 0 && sample < source_size) {
      frame_samples[static_cast<size_t>(n)] = source[static_cast<size_t>(sample)];
    }
  }

  difference.assign(static_cast<size_t>(lag_high - lag_low + 1), 0.0);
  size_t best = 0;
  for (int lag = lag_low; lag <= lag_high; ++lag) {
    double sum = 0.0;
    for (int n = 0; n < window; ++n) {
      const double delta = static_cast<double>(frame_samples[static_cast<size_t>(n)]) -
                           static_cast<double>(frame_samples[static_cast<size_t>(n + lag)]);
      sum += delta * delta;
    }
    const size_t index = static_cast<size_t>(lag - lag_low);
    difference[index] = sum;
    if (sum < difference[best]) best = index;
  }
  if (best == 0 || best + 1 == difference.size()) return f0_hz;
  const double left = difference[best - 1];
  const double middle = difference[best];
  const double right = difference[best + 1];
  const double denominator = left - 2.0 * middle + right;
  if (!(denominator > 0.0)) return f0_hz;
  const double offset = 0.5 * (left - right) / denominator;
  const double refined_period = static_cast<double>(lag_low) + static_cast<double>(best) + offset;
  const double refined = sample_rate / refined_period;
  if (!std::isfinite(refined) || refined < f0_hz / ratio || refined > f0_hz * ratio) return f0_hz;
  return static_cast<float>(refined);
}

}  // namespace sonare::editing::pitch_editor::detail
