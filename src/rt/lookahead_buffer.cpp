#include "rt/lookahead_buffer.h"

#include <cmath>

namespace sonare::rt {

void LookaheadBuffer::prepare(size_t lookahead_samples) {
  lookahead_samples_ = lookahead_samples;
  delay_.prepare(lookahead_samples_);
  window_peak_.prepare(lookahead_samples_ + 1);
  reset();
}

void LookaheadBuffer::reset() {
  delay_.reset();
  window_peak_.reset();
  peak_ = 0.0f;
}

float LookaheadBuffer::process(float input) {
  window_peak_.push(std::abs(input));
  peak_ = window_peak_.max();

  return delay_.process(input);
}

}  // namespace sonare::rt
