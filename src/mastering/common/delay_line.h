#pragma once

/// @file delay_line.h
/// @brief Fixed-size mono delay line.

#include <cstddef>
#include <vector>

namespace sonare::mastering::common {

class DelayLine {
 public:
  void prepare(size_t delay_samples);
  void reset();
  float process(float input);
  size_t delay_samples() const { return delay_samples_; }

 private:
  std::vector<float> buffer_{0.0f};
  size_t delay_samples_ = 0;
  size_t write_index_ = 0;
};

}  // namespace sonare::mastering::common
