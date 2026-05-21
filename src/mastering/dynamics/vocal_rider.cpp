#include "mastering/dynamics/vocal_rider.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

VocalRider::VocalRider(VocalRiderConfig config) : config_(config) { validate_config(config_); }

void VocalRider::prepare(double sample_rate, int max_block_size) {
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

void VocalRider::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("VocalRider must be prepared before processing");
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
  float largest_abs_gain = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float level = follower.process(channels[ch][i]);
      const float target_gain_db = config_.target_db - linear_to_db(level);
      const float ride_db = std::clamp(target_gain_db, -config_.max_cut_db, config_.max_boost_db);
      const float gain_db = ride_db + config_.output_gain_db;
      channels[ch][i] *= db_to_linear(gain_db);
      if (std::abs(ride_db) > std::abs(largest_abs_gain)) {
        largest_abs_gain = ride_db;
      }
    }
  }

  last_gain_db_ = largest_abs_gain;
}

void VocalRider::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

void VocalRider::set_config(const VocalRiderConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : followers_) {
      follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
    }
    reset();
  }
}

void VocalRider::validate_config(const VocalRiderConfig& config) {
  if (config.max_boost_db < 0.0f || config.max_cut_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f) {
    throw std::invalid_argument("invalid vocal rider configuration");
  }
}

float VocalRider::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

float VocalRider::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

void VocalRider::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
