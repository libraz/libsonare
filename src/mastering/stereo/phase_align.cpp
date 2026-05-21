#include "mastering/stereo/phase_align.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mastering::stereo {

PhaseAlign::PhaseAlign(PhaseAlignConfig config) : config_(config) { validate_config(config_); }

void PhaseAlign::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  prepared_ = true;
  rebuild_delay();
}

void PhaseAlign::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("PhaseAlign must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
  if (num_channels < 2 || config_.delay_samples == 0) {
    return;
  }

  const int delayed_ch = config_.delay_right ? 1 : 0;
  for (int i = 0; i < num_samples; ++i) {
    channels[delayed_ch][i] = process_delay(channels[delayed_ch][i]);
  }
}

void PhaseAlign::reset() {
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  delay_index_ = 0;
}

void PhaseAlign::set_config(const PhaseAlignConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    rebuild_delay();
  }
}

void PhaseAlign::validate_config(const PhaseAlignConfig& config) {
  if (config.delay_samples < 0) {
    throw std::invalid_argument("phase align delay must be non-negative");
  }
}

void PhaseAlign::rebuild_delay() {
  delay_.assign(static_cast<size_t>(std::max(config_.delay_samples, 1)), 0.0f);
  delay_index_ = 0;
}

float PhaseAlign::process_delay(float input) {
  float delayed = delay_[delay_index_];
  delay_[delay_index_] = input;
  delay_index_ = (delay_index_ + 1) % delay_.size();
  return delayed;
}

}  // namespace sonare::mastering::stereo
