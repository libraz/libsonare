#pragma once

/// @file mod_delay_line.h
/// @brief Fractional delay line with linear interpolation.

#include <vector>

namespace sonare::effects::modulation {

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
