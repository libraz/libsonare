#include "mastering/common/lookahead_buffer.h"

#include <algorithm>
#include <cmath>

namespace sonare::mastering::common {

void LookaheadBuffer::prepare(size_t lookahead_samples) {
  lookahead_samples_ = lookahead_samples;
  delay_.prepare(lookahead_samples_);
  reset();
}

void LookaheadBuffer::reset() {
  delay_.reset();
  window_.clear();
  peak_ = 0.0f;
}

float LookaheadBuffer::process(float input) {
  window_.push_back(std::abs(input));
  if (window_.size() > lookahead_samples_ + 1) {
    window_.pop_front();
  }

  peak_ = 0.0f;
  for (float sample : window_) {
    peak_ = std::max(peak_, sample);
  }

  return delay_.process(input);
}

}  // namespace sonare::mastering::common
