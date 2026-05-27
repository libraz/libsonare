#include "mixing/alignment_delay.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mixing {

AlignmentDelay::AlignmentDelay(int delay_samples)
    : delay_samples_(std::max(0, delay_samples)),
      delay_samples_q8_(std::max(0, delay_samples) << 8) {}

void AlignmentDelay::prepare(double, int) {
  prepared_channels_ = std::max(prepared_channels_, 2);
  prepare_storage();
}

void AlignmentDelay::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // Do not allocate on the audio thread: process only the channels prepared
  // for (clamp to the prepared count). prepare() preallocates the maximum.
  const int channels_to_process = std::min(num_channels, prepared_channels_);
  for (int ch = 0; ch < channels_to_process; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    if (fractional_mode_ == FractionalDelayMode::None || (delay_samples_q8_ & 0xff) == 0) {
      rt::DelayLine& delay = delays_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = delay.process(channels[ch][i]);
      }
    } else {
      FractionalState& state = fractional_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = process_fractional(state, channels[ch][i]);
      }
    }
  }
}

void AlignmentDelay::reset() {
  for (rt::DelayLine& delay : delays_) {
    delay.reset();
  }
  for (FractionalState& state : fractional_) {
    std::fill(state.buffer.begin(), state.buffer.end(), 0.0f);
    state.write_index = 0;
  }
}

void AlignmentDelay::set_delay_samples(int delay_samples) {
  delay_samples_ = std::max(0, delay_samples);
  delay_samples_q8_ = delay_samples_ << 8;
  fractional_mode_ = FractionalDelayMode::None;
  prepare_storage();
}

void AlignmentDelay::set_delay_samples_q8(int delay_samples_q8, FractionalDelayMode mode) {
  delay_samples_q8_ = std::max(0, delay_samples_q8);
  delay_samples_ = delay_samples_q8_ >> 8;
  fractional_mode_ = (delay_samples_q8_ & 0xff) == 0 ? FractionalDelayMode::None : mode;
  prepare_storage();
}

void AlignmentDelay::prepare_storage() {
  if (prepared_channels_ <= 0) {
    return;
  }

  delays_.assign(static_cast<size_t>(prepared_channels_), rt::DelayLine{});
  for (rt::DelayLine& delay : delays_) {
    delay.prepare(static_cast<size_t>(delay_samples_));
  }

  const int integer_delay = delay_samples_q8_ >> 8;
  const size_t fractional_size = static_cast<size_t>(std::max(8, integer_delay + 8));
  fractional_.assign(static_cast<size_t>(prepared_channels_), FractionalState{});
  for (FractionalState& state : fractional_) {
    state.buffer.assign(fractional_size, 0.0f);
    state.write_index = 0;
  }
}

float AlignmentDelay::process_fractional(FractionalState& state, float input) const noexcept {
  auto sample_at_delay = [&](int delay) {
    delay = std::max(0, delay);
    const size_t size = state.buffer.size();
    const size_t index = (state.write_index + size - (static_cast<size_t>(delay) % size)) % size;
    return state.buffer[index];
  };

  state.buffer[state.write_index] = input;
  const float delay = static_cast<float>(delay_samples_q8_) / 256.0f;
  const int base = static_cast<int>(std::floor(delay));
  const float mu = delay - static_cast<float>(base);

  const float y0 = sample_at_delay(base - 1);
  const float y1 = sample_at_delay(base);
  const float y2 = sample_at_delay(base + 1);
  const float y3 = sample_at_delay(base + 2);

  state.write_index = (state.write_index + 1) % state.buffer.size();

  const float c0 = -mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
  const float c1 = (mu + 1.0f) * (mu - 1.0f) * (mu - 2.0f) / 2.0f;
  const float c2 = -(mu + 1.0f) * mu * (mu - 2.0f) / 2.0f;
  const float c3 = (mu + 1.0f) * mu * (mu - 1.0f) / 6.0f;
  return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
}

}  // namespace sonare::mixing
