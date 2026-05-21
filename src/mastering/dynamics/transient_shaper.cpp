#include "mastering/dynamics/transient_shaper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

TransientShaper::TransientShaper(TransientShaperConfig config) : config_(config) {
  validate_config(config_);
}

void TransientShaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  for (auto& follower : fast_followers_) {
    follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
  }
  for (auto& follower : slow_followers_) {
    follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
  }
  reset();
}

void TransientShaper::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("TransientShaper must be prepared before processing");
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

    auto& fast = fast_followers_[static_cast<size_t>(ch)];
    auto& slow = slow_followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float fast_env = fast.process(channels[ch][i]);
      const float slow_env = slow.process(channels[ch][i]);
      const float diff = fast_env - slow_env;
      const float denom = std::max(std::max(fast_env, slow_env), 0.000001f);
      const float amount = std::clamp(std::abs(diff) / denom * config_.sensitivity, 0.0f, 1.0f);
      const float target_db = diff >= 0.0f ? config_.attack_gain_db : config_.sustain_gain_db;
      const float gain_db =
          std::clamp(target_db * amount, -config_.max_gain_db, config_.max_gain_db);

      channels[ch][i] *= db_to_linear(gain_db);
      if (std::abs(gain_db) > std::abs(largest_abs_gain)) {
        largest_abs_gain = gain_db;
      }
    }
  }

  last_gain_db_ = largest_abs_gain;
}

void TransientShaper::reset() {
  for (auto& follower : fast_followers_) {
    follower.reset();
  }
  for (auto& follower : slow_followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

void TransientShaper::set_config(const TransientShaperConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    for (auto& follower : fast_followers_) {
      follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
    }
    for (auto& follower : slow_followers_) {
      follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
    }
    reset();
  }
}

void TransientShaper::validate_config(const TransientShaperConfig& config) {
  if (config.fast_attack_ms < 0.0f || config.fast_release_ms < 0.0f ||
      config.slow_attack_ms < 0.0f || config.slow_release_ms < 0.0f || config.sensitivity < 0.0f ||
      config.max_gain_db < 0.0f) {
    throw std::invalid_argument("invalid transient shaper configuration");
  }
}

float TransientShaper::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

void TransientShaper::ensure_followers(int num_channels) {
  if (fast_followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  fast_followers_.assign(static_cast<size_t>(num_channels), {});
  slow_followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : fast_followers_) {
    follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
  }
  for (auto& follower : slow_followers_) {
    follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
