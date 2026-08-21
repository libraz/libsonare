#pragma once

/// @file mod_delay_line.h
/// @brief Fractional delay line with linear interpolation.

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

class ModDelayLine {
 public:
  void prepare(int max_delay_samples);
  void reset();
  float process(float input, float delay_samples);
  int max_delay_samples() const noexcept { return max_delay_samples_; }

 private:
  std::vector<float> buffer_{0.0f};
  int max_delay_samples_ = 0;
  int write_index_ = 0;
};

}  // namespace sonare::effects::modulation
