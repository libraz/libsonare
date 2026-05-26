#include "mastering/dynamics/expander.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"

namespace sonare::mastering::dynamics {

Expander::Expander(ExpanderConfig config) : config_(config) { validate_config(config_); }

void Expander::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
  reset();
}

void Expander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("Expander must be prepared before processing");
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
  float min_reduction = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float envelope = follower.process(channels[ch][i]);
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), config_);
      channels[ch][i] *= db_to_linear(reduction_db);
      min_reduction = std::min(min_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = min_reduction;
}

void Expander::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

void Expander::set_config(const ExpanderConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

bool Expander::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      return true;
    case 1:
      config_.ratio = std::max(1.0f, value);
      return true;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      // Recompute follower coefficients in place; preserves envelope state.
      for (auto& follower : followers_) {
        follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
      }
      return true;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      for (auto& follower : followers_) {
        follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
      }
      return true;
    case 4:
      config_.range_db = std::min(0.0f, value);
      return true;
    default:
      return false;
  }
}

void Expander::validate_config(const ExpanderConfig& config) {
  if (!(config.ratio >= 1.0f)) {
    throw std::invalid_argument("expander ratio must be at least 1");
  }
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f) {
    throw std::invalid_argument("invalid expander configuration");
  }
}

float Expander::gain_reduction_db(float input_db, const ExpanderConfig& config) {
  if (input_db >= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float below_db = config.threshold_db - input_db;
  const float reduction = below_db * (config.ratio - 1.0f);
  return std::max(config.range_db, -reduction);
}

void Expander::ensure_followers(int num_channels) {
  if (followers_.size() >= static_cast<size_t>(num_channels)) {
    return;
  }

  throw std::invalid_argument("num_channels exceeds prepared Expander state");
}

}  // namespace sonare::mastering::dynamics
