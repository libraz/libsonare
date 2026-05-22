#include "mastering/dynamics/upward_expander.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/db.h"

namespace sonare::mastering::dynamics {

UpwardExpander::UpwardExpander(UpwardExpanderConfig config) : config_(config) {
  validate_config(config_);
}

void UpwardExpander::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  reset();
}

void UpwardExpander::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("UpwardExpander must be prepared before processing");
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

  ensure_followers(num_channels);
  float max_gain = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float envelope = follower.process(channels[ch][i]);
      const float applied_gain_db = gain_db(linear_to_db(envelope), config_);
      channels[ch][i] *= db_to_linear(applied_gain_db);
      max_gain = std::max(max_gain, applied_gain_db);
    }
  }

  last_gain_db_ = max_gain;
}

void UpwardExpander::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

void UpwardExpander::set_config(const UpwardExpanderConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

void UpwardExpander::validate_config(const UpwardExpanderConfig& config) {
  if (!(config.ratio >= 1.0f) || config.range_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f) {
    throw std::invalid_argument("invalid upward expander configuration");
  }
}

float UpwardExpander::gain_db(float input_db, const UpwardExpanderConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  const float gain = over_db * (config.ratio - 1.0f);
  return std::min(config.range_db, gain);
}

void UpwardExpander::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
