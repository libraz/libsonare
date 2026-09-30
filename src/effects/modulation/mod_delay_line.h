#pragma once

/// @file mod_delay_line.h
/// @brief Fractional delay line with linear or third-order Lagrange interpolation.

#include <cmath>
#include <vector>

namespace sonare::effects::modulation {

/// @brief True when a delay-related parameter value can safely reach a tap.
///
/// std::clamp cannot express this: every comparison against NaN is false, so it
/// returns NaN unchanged, and the value then lands in a fractional read index
/// where `static_cast<int>(std::floor(NaN))` is undefined and the interpolation
/// indexes the buffer out of bounds. The delay-based processors call this at the
/// top of set_parameter, so an unusable request leaves the previous value in
/// place instead of poisoning the line; the taps themselves re-check it as a
/// last line of defence for a directly-constructed config.
inline bool delay_param_acceptable(float value) noexcept { return std::isfinite(value); }

/// How a fractional delay is read between two stored samples.
enum class DelayInterpolation {
  kLinear,  ///< Two-point read; droops the top of the band by an amount that follows the fraction.
  kLagrange3,  ///< Four-point Lagrange read; flatter at high frequencies, delay floor of 1 sample.
};
inline constexpr int kDelayInterpolationCount = 2;

/// True when @p value names a DelayInterpolation, for a realtime setter that refuses the rest.
inline bool delay_interpolation_acceptable(float value) noexcept {
  return value >= 0.0f && value == std::floor(value) &&
         value < static_cast<float>(kDelayInterpolationCount);
}

class ModDelayLine {
 public:
  void prepare(int max_delay_samples);
  void reset();
  float process(float input, float delay_samples);

  /// Selects the read. Lagrange reads the four samples around the delay, so it clamps the delay to
  /// [1, max - 1]: a delay of zero would need a sample that has not been written yet.
  void set_interpolation(DelayInterpolation interpolation) noexcept {
    interpolation_ = interpolation;
  }
  DelayInterpolation interpolation() const noexcept { return interpolation_; }
  int max_delay_samples() const noexcept { return max_delay_samples_; }

 private:
  float process_lagrange3(float input, float delay_samples);

  std::vector<float> buffer_{0.0f};
  int max_delay_samples_ = 0;
  int write_index_ = 0;
  DelayInterpolation interpolation_ = DelayInterpolation::kLinear;
};

}  // namespace sonare::effects::modulation
