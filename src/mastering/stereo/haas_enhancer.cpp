#include "mastering/stereo/haas_enhancer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"

namespace sonare::mastering::stereo {

HaasEnhancer::HaasEnhancer(HaasEnhancerConfig config) : config_(config) {
  validate_config(config_);
}

void HaasEnhancer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  rebuild_delay();
}

void HaasEnhancer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "HaasEnhancer");
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
  if (num_channels < 2 || delay_samples_ == 0 || config_.mix == 0.0f) {
    return;
  }

  const int delayed_ch = config_.delay_right ? 1 : 0;
  const int source_ch = config_.delay_right ? 0 : 1;
  for (int i = 0; i < num_samples; ++i) {
    const float delayed = process_delay(channels[source_ch][i]);
    channels[delayed_ch][i] =
        channels[delayed_ch][i] * (1.0f - config_.mix) + delayed * config_.mix;
  }
}

void HaasEnhancer::reset() {
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  delay_index_ = 0;
}

void HaasEnhancer::set_config(const HaasEnhancerConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    rebuild_delay();
  }
}

bool HaasEnhancer::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.delay_ms = std::max(0.0f, value);
      // Changing the delay length requires resizing the delay line, which
      // reallocates and clears the buffered samples.
      if (prepared_) {
        rebuild_delay();
      }
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

bool HaasEnhancer::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // delay_ms reallocates the delay line; everything else is an in-place scalar.
  return param_id != 0;
}

void HaasEnhancer::validate_config(const HaasEnhancerConfig& config) {
  if (config.delay_ms < 0.0f || config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("invalid Haas enhancer configuration");
  }
}

void HaasEnhancer::rebuild_delay() {
  delay_samples_ = static_cast<int>(std::round(sample_rate_ * config_.delay_ms * 0.001));
  delay_.assign(static_cast<size_t>(std::max(delay_samples_, 1)), 0.0f);
  delay_index_ = 0;
}

float HaasEnhancer::process_delay(float input) {
  float delayed = delay_[delay_index_];
  delay_[delay_index_] = input;
  delay_index_ = (delay_index_ + 1) % delay_.size();
  return delayed;
}

}  // namespace sonare::mastering::stereo
