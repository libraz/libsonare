#include "rt/delay_line.h"

#include <algorithm>

namespace sonare::rt {

void DelayLine::prepare(size_t delay_samples) {
  delay_samples_ = delay_samples;
  buffer_.assign(std::max<size_t>(delay_samples, 1), 0.0f);
  write_index_ = 0;
}

void DelayLine::reset() {
  std::fill(buffer_.begin(), buffer_.end(), 0.0f);
  write_index_ = 0;
}

float DelayLine::process(float input) {
  if (delay_samples_ == 0) return input;

  const float output = buffer_[write_index_];
  buffer_[write_index_] = input;
  write_index_ = (write_index_ + 1) % buffer_.size();
  return output;
}

}  // namespace sonare::rt
