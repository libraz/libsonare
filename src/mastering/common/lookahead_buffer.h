#pragma once

/// @file lookahead_buffer.h
/// @brief Peak lookahead helper for limiter-style processors.

#include <cstddef>
#include <deque>

#include "mastering/common/delay_line.h"

namespace sonare::mastering::common {

class LookaheadBuffer {
 public:
  void prepare(size_t lookahead_samples);
  void reset();
  float process(float input);
  float peak() const { return peak_; }

 private:
  DelayLine delay_;
  std::deque<float> window_;
  size_t lookahead_samples_ = 0;
  float peak_ = 0.0f;
};

}  // namespace sonare::mastering::common
