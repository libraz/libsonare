#include "mastering/spectral/presence_enhancer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::spectral {

PresenceEnhancer::PresenceEnhancer(PresenceEnhancerConfig config) : config_(config) {
  validate_config(config_);
}

void PresenceEnhancer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  prepared_ = true;
}

void PresenceEnhancer::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("PresenceEnhancer must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      const float harmonic = std::tanh(channels[ch][i] * config_.drive);
      channels[ch][i] = std::clamp(channels[ch][i] + harmonic * config_.amount, -1.5f, 1.5f);
    }
  }
}

void PresenceEnhancer::set_config(const PresenceEnhancerConfig& config) {
  validate_config(config);
  config_ = config;
}

void PresenceEnhancer::validate_config(const PresenceEnhancerConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f) || !(config.drive > 0.0f)) {
    throw std::invalid_argument("invalid presence enhancer configuration");
  }
}

}  // namespace sonare::mastering::spectral
