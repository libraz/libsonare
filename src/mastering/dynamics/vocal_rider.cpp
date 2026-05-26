#include "mastering/dynamics/vocal_rider.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

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
  sonare::mastering::common::ScopedNoDenormals guard;
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
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }
  const float smoothing = coeff(sample_rate_, config_.gain_smoothing_ms);
  if (config_.linked_detection) {
    for (int i = 0; i < num_samples; ++i) {
      float linked_level = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        linked_level =
            std::max(linked_level, followers_[static_cast<size_t>(ch)].process(channels[ch][i]));
      }
      const float level_db = linear_to_db(linked_level);
      float ride_db = 0.0f;
      if (level_db >= config_.noise_floor_db) {
        const float target_gain_db = config_.target_db - level_db;
        ride_db = std::clamp(target_gain_db, -config_.max_cut_db, config_.max_boost_db);
      }
      linked_gain_state_db_ = smoothing * linked_gain_state_db_ + (1.0f - smoothing) * ride_db;
      const float gain_db = linked_gain_state_db_ + config_.output_gain_db;
      const float gain = db_to_linear(gain_db);
      for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] *= gain;
      if (std::abs(linked_gain_state_db_) > std::abs(largest_abs_gain)) {
        largest_abs_gain = linked_gain_state_db_;
      }
    }
  } else {
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& follower = followers_[static_cast<size_t>(ch)];
      float& gain_state = unlinked_gain_state_db_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        const float level = follower.process(channels[ch][i]);
        const float level_db = linear_to_db(level);
        float ride_db = 0.0f;
        if (level_db >= config_.noise_floor_db) {
          const float target_gain_db = config_.target_db - level_db;
          ride_db = std::clamp(target_gain_db, -config_.max_cut_db, config_.max_boost_db);
        }
        gain_state = smoothing * gain_state + (1.0f - smoothing) * ride_db;
        const float gain_db = gain_state + config_.output_gain_db;
        channels[ch][i] *= db_to_linear(gain_db);
        if (std::abs(ride_db) > std::abs(largest_abs_gain)) largest_abs_gain = ride_db;
      }
    }
  }

  last_gain_db_ = largest_abs_gain;
}

void VocalRider::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  linked_gain_state_db_ = 0.0f;
  std::fill(unlinked_gain_state_db_.begin(), unlinked_gain_state_db_.end(), 0.0f);
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

bool VocalRider::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.target_db = value;
      return true;
    case 1:
      config_.max_boost_db = std::max(0.0f, value);
      return true;
    case 2:
      config_.max_cut_db = std::max(0.0f, value);
      return true;
    case 3:
      config_.attack_ms = std::max(0.0f, value);
      // Recompute follower coefficients in place; preserves envelope state.
      if (prepared_) {
        for (auto& follower : followers_) {
          follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 4:
      config_.release_ms = std::max(0.0f, value);
      if (prepared_) {
        for (auto& follower : followers_) {
          follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
        }
      }
      return true;
    case 5:
      config_.output_gain_db = value;
      return true;
    case 6:
      // The smoothing coefficient is derived per sample from this value, so a
      // plain update is RT-safe and preserves the running gain state.
      config_.gain_smoothing_ms = std::max(0.0f, value);
      return true;
    case 7:
      config_.noise_floor_db = value;
      return true;
    default:
      return false;
  }
}

void VocalRider::validate_config(const VocalRiderConfig& config) {
  if (config.max_boost_db < 0.0f || config.max_cut_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f || config.gain_smoothing_ms < 0.0f) {
    throw std::invalid_argument("invalid vocal rider configuration");
  }
}

float VocalRider::coeff(double sample_rate, float ms) {
  return time_to_coefficient(sample_rate, ms);
}

void VocalRider::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    if (unlinked_gain_state_db_.size() != static_cast<size_t>(num_channels)) {
      unlinked_gain_state_db_.assign(static_cast<size_t>(num_channels), 0.0f);
    }
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  unlinked_gain_state_db_.assign(static_cast<size_t>(num_channels), 0.0f);
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
