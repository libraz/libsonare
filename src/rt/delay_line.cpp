#include "rt/delay_line.h"

#include <algorithm>

namespace sonare::rt {

void DelayLine::prepare(size_t delay_samples) {
  // Build the replacement first. Apart from giving the control-thread caller
  // the usual strong exception guarantee, this is important for zero delay:
  // a pass-through line must not retain the old ring's capacity.
  std::vector<float> next(delay_samples, 0.0f);
  buffer_.swap(next);
  delay_samples_ = delay_samples;
  write_index_ = 0;
}

void DelayLine::reset() noexcept {
  std::fill(buffer_.begin(), buffer_.end(), 0.0f);
  write_index_ = 0;
}

float DelayLine::process(float input) noexcept {
  if (delay_samples_ == 0) return input;

  const float output = buffer_[write_index_];
  buffer_[write_index_] = input;
  write_index_ = (write_index_ + 1) % buffer_.size();
  return output;
}

}  // namespace sonare::rt
