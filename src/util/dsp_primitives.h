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

/// @brief Convert a pitch interval in cents to a linear frequency ratio.
/// @details Returns `2^(cents / 1200)` via `std::exp2`, matching the physical-model
///   synth voices' historical arithmetic bit-for-bit (float throughout). Callers
///   that need a `double` result or that fold the divide into `* (1 / 1200)` keep
///   their own expression, since the rounding differs.
inline float cents_to_ratio(float cents) noexcept {
  return std::exp2(cents / constants::kCentsPerOctave);
}

/// @brief Phase delay, in samples, of a one-pole filter `y[n] = x[n] + a*y[n-1]`
///   evaluated at angular frequency `omega`.
/// @details Returns `atan2(a*sin(w), 1 - a*cos(w)) / max(w, 1e-6)`. Used to
///   compensate the tuning-filter loop delay of the waveguide/comb voices;
///   matches their inlined arithmetic bit-for-bit (float throughout). The
///   denominator is floored at `1e-6` so a zero frequency does not divide by zero.
inline float onepole_group_delay_samples(float a, float omega) noexcept {
  return std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
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

/// @brief Mean square over the clipped centered window `[c - radius, c + radius]` of a
///   buffer, kept as a running sum while the center moves.
/// @details A query adds and removes only the samples that entered or left the previous
///   window, and resums from scratch after one window length of edge travel so the running
///   double sum cannot drift; a scan over adjacent centers therefore reads each sample a
///   bounded number of times regardless of the radius.
class CenteredWindowEnergy {
 public:
  CenteredWindowEnergy(const float* data, size_t size, size_t radius) noexcept
      : data_(data), size_(size), radius_(radius), resync_travel_(2 * radius + 1) {}

  /// @pre `center < size`.
  double mean_square_at(size_t center) noexcept {
    const size_t begin = center > radius_ ? center - radius_ : 0;
    const size_t end = std::min(size_, center + radius_ + 1);
    const bool overlaps = valid_ && begin < end_ && begin_ < end;
    if (!overlaps || travel_ >= resync_travel_) {
      sum_ = accumulate(begin, end);
      travel_ = 0;
      valid_ = true;
    } else {
      travel_ += (begin > begin_ ? begin - begin_ : begin_ - begin) +
                 (end > end_ ? end - end_ : end_ - end);
      if (begin > begin_) {
        sum_ -= accumulate(begin_, begin);
      } else {
        sum_ += accumulate(begin, begin_);
      }
      if (end > end_) {
        sum_ += accumulate(end_, end);
      } else {
        sum_ -= accumulate(end, end_);
      }
    }
    begin_ = begin;
    end_ = end;
    return std::max(0.0, sum_) / static_cast<double>(end - begin);
  }

  /// @brief Samples read since construction; the work a scan costs.
  size_t samples_read() const noexcept { return samples_read_; }

 private:
  double accumulate(size_t from, size_t to) noexcept {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) {
      sum += static_cast<double>(data_[i]) * static_cast<double>(data_[i]);
    }
    samples_read_ += to - from;
    return sum;
  }

  const float* data_;
  size_t size_;
  size_t radius_;
  size_t resync_travel_;
  size_t begin_ = 0;
  size_t end_ = 0;
  size_t travel_ = 0;
  size_t samples_read_ = 0;
  double sum_ = 0.0;
  bool valid_ = false;
};

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

/// @brief Equal-power (constant-energy) crossfade between two samples.
/// @details Uses the square-root law `a*sqrt(1-x) + b*sqrt(x)` whose gains
///   satisfy `g_a^2 + g_b^2 = 1`, so two decorrelated sources sum to constant
///   energy across the fade (no level dip at the seam). `mix` is clamped to
///   [0, 1]. Note: the sine/cosine fade-gain law used by some fade-curve
///   enums (`sin(pi/2*x)`) is a distinct equal-power formulation and is kept
///   separate where golden output depends on it.
inline float equal_power_crossfade(float a, float b, float mix) noexcept {
  const float x = std::clamp(mix, 0.0f, 1.0f);
  return a * std::sqrt(1.0f - x) + b * std::sqrt(x);
}

}  // namespace sonare
