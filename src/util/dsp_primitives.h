#pragma once

/// @file dsp_primitives.h
/// @brief Small reusable DSP primitives.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "util/constants.h"

namespace sonare {

/// @brief Convert a time constant in milliseconds to a one-pole IIR coefficient.
/// @details Returns exp(-1 / (sample_rate * time_ms / 1000)).
inline float time_to_coefficient(double sample_rate, float time_ms) noexcept {
  if (time_ms <= 0.0f || sample_rate <= 0.0) {
    return 0.0f;
  }
  const double samples = sample_rate * static_cast<double>(time_ms) * 0.001;
  return static_cast<float>(std::exp(-1.0 / samples));
}

/// @brief Convert a time constant in milliseconds to a leaky-integrator rate.
/// @details Returns `1 - exp(-1 / max(sample_rate * time_ms / 1000, 1))`. This
///   is the "new sample weight" used in attack/release envelope followers of
///   the form `y[n] = y[n-1] + rate * (x[n] - y[n-1])`. The denominator is
///   floored at one sample so sub-sample time constants don't blow up the
///   exponent. Returns 1.0 (instantaneous follow) for time_ms <= 0.
inline double time_to_attack_release_rate(double sample_rate, float time_ms) noexcept {
  if (time_ms <= 0.0f) {
    return 1.0;
  }
  const double samples = std::max(sample_rate * static_cast<double>(time_ms) * 0.001, 1.0);
  return 1.0 - std::exp(-1.0 / samples);
}

/// @brief `float` overload of @ref time_to_attack_release_rate.
/// @details Computes the same leaky-integrator "new sample weight" as the
///   `double` version (`1 - exp(-1 / max(sample_rate * time_ms / 1000, 1))`)
///   but returns it as `float`. Returns 1.0f (instantaneous follow) for
///   time_ms <= 0 or sample_rate <= 0, so callers do not need their own
///   degenerate-input guards before invoking it.
inline float time_to_attack_release_rate_f(double sample_rate, float time_ms) noexcept {
  if (time_ms <= 0.0f || sample_rate <= 0.0) {
    return 1.0f;
  }
  const double samples = std::max(sample_rate * static_cast<double>(time_ms) * 0.001, 1.0);
  return static_cast<float>(1.0 - std::exp(-1.0 / samples));
}

/// @brief Root mean square of a contiguous sample buffer.
inline float rms(const float* data, size_t n) noexcept {
  if (data == nullptr || n == 0) {
    return 0.0f;
  }
  double sum_sq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum_sq += static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }
  return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n)));
}

/// @brief Peak absolute amplitude of a contiguous sample buffer.
inline float peak_abs(const float* data, size_t n) noexcept {
  if (data == nullptr || n == 0) {
    return 0.0f;
  }
  float peak = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    peak = std::max(peak, std::abs(data[i]));
  }
  return peak;
}

/// @brief Half-cosine fade-in gain for one sample index.
inline float cosine_fade_in_gain(size_t index, size_t length) noexcept {
  if (length == 0) {
    return 1.0f;
  }
  const float t = static_cast<float>(index) / static_cast<float>(length);
  return 0.5f * (1.0f - std::cos(constants::kPi * t));
}

/// @brief Half-cosine fade-out gain for one sample index.
inline float cosine_fade_out_gain(size_t index, size_t length) noexcept {
  if (length == 0) {
    return 1.0f;
  }
  const float t = static_cast<float>(index) / static_cast<float>(length);
  return 0.5f * (1.0f + std::cos(constants::kPi * t));
}

/// @brief Linear crossfade between two samples.
inline float linear_crossfade(float a, float b, float mix) noexcept {
  return a * (1.0f - mix) + b * mix;
}

}  // namespace sonare
