#include "effects/modulation/mod_delay_line.h"

#include <algorithm>
#include <cmath>

namespace sonare::effects::modulation {

void ModDelayLine::prepare(int max_delay_samples) {
  max_delay_samples_ = std::max(1, max_delay_samples);
  buffer_.assign(static_cast<size_t>(max_delay_samples_ + 2), 0.0f);
  write_index_ = 0;
}

void ModDelayLine::reset() {
  std::fill(buffer_.begin(), buffer_.end(), 0.0f);
  write_index_ = 0;
}

float ModDelayLine::process(float input, float delay_samples) {
  if (buffer_.empty()) {
    prepare(1);
  }
  const float clamped_delay =
      std::clamp(delay_samples, 0.0f, static_cast<float>(max_delay_samples_));
  buffer_[static_cast<size_t>(write_index_)] = input;

  float read_position = static_cast<float>(write_index_) - clamped_delay;
  const float size = static_cast<float>(buffer_.size());
  while (read_position < 0.0f) {
    read_position += size;
  }

  const int index0 = static_cast<int>(std::floor(read_position)) % static_cast<int>(buffer_.size());
  const int index1 = (index0 + 1) % static_cast<int>(buffer_.size());
  const float frac = read_position - std::floor(read_position);
  const float output = buffer_[static_cast<size_t>(index0)] * (1.0f - frac) +
                       buffer_[static_cast<size_t>(index1)] * frac;

  write_index_ = (write_index_ + 1) % static_cast<int>(buffer_.size());
  return output;
}

}  // namespace sonare::effects::modulation
