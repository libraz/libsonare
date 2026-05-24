#include "mixing/alignment_delay.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mixing {

AlignmentDelay::AlignmentDelay(int delay_samples) : delay_samples_(std::max(0, delay_samples)) {}

void AlignmentDelay::prepare(double, int) {
  prepared_channels_ = std::max(prepared_channels_, 2);
  delays_.assign(static_cast<size_t>(prepared_channels_), rt::DelayLine{});
  for (rt::DelayLine& delay : delays_) {
    delay.prepare(static_cast<size_t>(delay_samples_));
  }
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
    rt::DelayLine& delay = delays_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = delay.process(channels[ch][i]);
    }
  }
}

void AlignmentDelay::reset() {
  for (rt::DelayLine& delay : delays_) {
    delay.reset();
  }
}

void AlignmentDelay::set_delay_samples(int delay_samples) {
  delay_samples_ = std::max(0, delay_samples);
  for (rt::DelayLine& delay : delays_) {
    delay.prepare(static_cast<size_t>(delay_samples_));
  }
}

}  // namespace sonare::mixing
