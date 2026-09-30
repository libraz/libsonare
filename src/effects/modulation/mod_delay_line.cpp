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
  if (!delay_param_acceptable(delay_samples)) {
    // No in-range read index exists for a non-finite delay, so the tap is
    // skipped entirely rather than computing one. The write head still advances
    // so the line stays time-coherent once a usable delay returns.
    buffer_[static_cast<size_t>(write_index_)] = input;
    write_index_ = (write_index_ + 1) % static_cast<int>(buffer_.size());
    return 0.0f;
  }
  if (interpolation_ == DelayInterpolation::kLagrange3) {
    return process_lagrange3(input, delay_samples);
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

float ModDelayLine::process_lagrange3(float input, float delay_samples) {
  // The far node sits two samples past the whole delay, so the ceiling is one below the line's.
  const float ceiling = static_cast<float>(std::max(1, max_delay_samples_ - 1));
  const float delay = std::clamp(delay_samples, 1.0f, ceiling);
  buffer_[static_cast<size_t>(write_index_)] = input;

  // Nodes sit at delays D-1, D, D+1, D+2 around the interval the fraction falls in.
  const int whole = static_cast<int>(delay);
  const float f = delay - static_cast<float>(whole);
  const float w0 = -f * (f - 1.0f) * (f - 2.0f) * (1.0f / 6.0f);
  const float w1 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
  const float w2 = -(f + 1.0f) * f * (f - 2.0f) * 0.5f;
  const float w3 = (f + 1.0f) * f * (f - 1.0f) * (1.0f / 6.0f);

  const int size = static_cast<int>(buffer_.size());
  auto tap = [&](int delay_back) {
    const int index = ((write_index_ - delay_back) % size + size) % size;
    return buffer_[static_cast<size_t>(index)];
  };
  const float output =
      w0 * tap(whole - 1) + w1 * tap(whole) + w2 * tap(whole + 1) + w3 * tap(whole + 2);

  write_index_ = (write_index_ + 1) % size;
  return output;
}

}  // namespace sonare::effects::modulation
