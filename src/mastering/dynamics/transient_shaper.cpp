#include "mastering/dynamics/transient_shaper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

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
  gain_smoothing_coeff_ = time_to_coefficient(sample_rate_, config_.gain_smoothing_ms);
  for (auto& follower : fast_followers_) {
    follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
  }
  for (auto& follower : slow_followers_) {
    follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
  }
  reset();
}

void TransientShaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
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
      auto idx = static_cast<size_t>(ch);
      const float smoothing = gain_smoothing_coeff_;
      gain_state_db_[idx] = smoothing * gain_state_db_[idx] + (1.0f - smoothing) * gain_db;
      float delayed = channels[ch][i];
      if (!lookahead_[idx].empty()) {
        delayed = lookahead_[idx][lookahead_index_[idx]];
        lookahead_[idx][lookahead_index_[idx]] = channels[ch][i];
        lookahead_index_[idx] = (lookahead_index_[idx] + 1) % lookahead_[idx].size();
      }
      channels[ch][i] = delayed * db_to_linear(gain_state_db_[idx]);
      if (std::abs(gain_state_db_[idx]) > std::abs(largest_abs_gain)) {
        largest_abs_gain = gain_state_db_[idx];
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
  std::fill(gain_state_db_.begin(), gain_state_db_.end(), 0.0f);
  for (auto& delay : lookahead_) std::fill(delay.begin(), delay.end(), 0.0f);
  std::fill(lookahead_index_.begin(), lookahead_index_.end(), 0);
  last_gain_db_ = 0.0f;
}

void TransientShaper::set_config(const TransientShaperConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    gain_smoothing_coeff_ = time_to_coefficient(sample_rate_, config_.gain_smoothing_ms);
    for (auto& follower : fast_followers_) {
      follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
    }
    for (auto& follower : slow_followers_) {
      follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
    }
    reset();
  }
}

bool TransientShaper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.attack_gain_db = value;
      return true;
    case 1:
      config_.sustain_gain_db = value;
      return true;
    case 2:
      config_.fast_attack_ms = std::max(0.0f, value);
      // Recompute fast-follower coefficients in place; preserves envelope state.
      if (prepared_) {
        for (auto& follower : fast_followers_) {
          follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
        }
      }
      return true;
    case 3:
      config_.fast_release_ms = std::max(0.0f, value);
      if (prepared_) {
        for (auto& follower : fast_followers_) {
          follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
        }
      }
      return true;
    case 4:
      config_.slow_attack_ms = std::max(0.0f, value);
      // Recompute slow-follower coefficients in place; preserves envelope state.
      if (prepared_) {
        for (auto& follower : slow_followers_) {
          follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
        }
      }
      return true;
    case 5:
      config_.slow_release_ms = std::max(0.0f, value);
      if (prepared_) {
        for (auto& follower : slow_followers_) {
          follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
        }
      }
      return true;
    case 6:
      config_.sensitivity = std::max(0.0f, value);
      return true;
    case 7:
      config_.max_gain_db = std::max(0.0f, value);
      return true;
    case 8:
      // Recompute the cached smoother coefficient in place; preserves the
      // running gain state. RT-safe (no allocation).
      config_.gain_smoothing_ms = std::max(0.0f, value);
      if (prepared_) {
        gain_smoothing_coeff_ = time_to_coefficient(sample_rate_, config_.gain_smoothing_ms);
      }
      return true;
    default:
      return false;
  }
}

void TransientShaper::validate_config(const TransientShaperConfig& config) {
  if (config.fast_attack_ms < 0.0f || config.fast_release_ms < 0.0f ||
      config.slow_attack_ms < 0.0f || config.slow_release_ms < 0.0f || config.sensitivity < 0.0f ||
      config.max_gain_db < 0.0f || config.gain_smoothing_ms < 0.0f || config.lookahead_ms < 0.0f) {
    throw std::invalid_argument("invalid transient shaper configuration");
  }
}

void TransientShaper::ensure_followers(int num_channels) {
  if (fast_followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  fast_followers_.assign(static_cast<size_t>(num_channels), {});
  slow_followers_.assign(static_cast<size_t>(num_channels), {});
  gain_state_db_.assign(static_cast<size_t>(num_channels), 0.0f);
  const size_t lookahead_samples =
      static_cast<size_t>(std::round(sample_rate_ * config_.lookahead_ms * 0.001));
  lookahead_.assign(static_cast<size_t>(num_channels), std::vector<float>(lookahead_samples, 0.0f));
  lookahead_index_.assign(static_cast<size_t>(num_channels), 0);
  for (auto& follower : fast_followers_) {
    follower.prepare(sample_rate_, config_.fast_attack_ms, config_.fast_release_ms);
  }
  for (auto& follower : slow_followers_) {
    follower.prepare(sample_rate_, config_.slow_attack_ms, config_.slow_release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
