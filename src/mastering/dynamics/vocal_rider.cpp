#include "mastering/dynamics/vocal_rider.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

VocalRider::VocalRider(VocalRiderConfig config)
    : config_(config), config_publisher_(std::make_unique<rt::RtPublisher<VocalRiderConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const VocalRiderConfig>(config_));
}

void VocalRider::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  update_coefficients(config_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const VocalRiderConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const VocalRiderConfig* VocalRider::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const VocalRiderConfig* current = config_publisher_->current();
  if (current && current != applied_snapshot_) {
    update_coefficients(*current);
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen on a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by prepared_ so this path stays defined.
  return current ? current : &config_;
}

void VocalRider::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "VocalRider");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }

  ensure_followers(num_channels);

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const VocalRiderConfig& cfg = *adopt_snapshot_for_block();

  float largest_abs_gain = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }
  const float smoothing = time_to_coefficient(sample_rate_, cfg.gain_smoothing_ms);
  if (cfg.linked_detection) {
    for (int i = 0; i < num_samples; ++i) {
      float linked_level = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        linked_level =
            std::max(linked_level, followers_[static_cast<size_t>(ch)].process(channels[ch][i]));
      }
      const float level_db = linear_to_db(linked_level);
      float ride_db = 0.0f;
      if (level_db >= cfg.noise_floor_db) {
        const float target_gain_db = cfg.target_db - level_db;
        ride_db = std::clamp(target_gain_db, -cfg.max_cut_db, cfg.max_boost_db);
      }
      linked_gain_state_db_ = smoothing * linked_gain_state_db_ + (1.0f - smoothing) * ride_db;
      const float gain_db = linked_gain_state_db_ + cfg.output_gain_db;
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
        if (level_db >= cfg.noise_floor_db) {
          const float target_gain_db = cfg.target_db - level_db;
          ride_db = std::clamp(target_gain_db, -cfg.max_cut_db, cfg.max_boost_db);
        }
        gain_state = smoothing * gain_state + (1.0f - smoothing) * ride_db;
        const float gain_db = gain_state + cfg.output_gain_db;
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
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const VocalRiderConfig>(config_));
}

bool VocalRider::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.target_db = value;
      break;
    case 1:
      config_.max_boost_db = std::max(0.0f, value);
      break;
    case 2:
      config_.max_cut_db = std::max(0.0f, value);
      break;
    case 3:
      config_.attack_ms = std::max(0.0f, value);
      break;
    case 4:
      config_.release_ms = std::max(0.0f, value);
      break;
    case 5:
      config_.output_gain_db = value;
      break;
    case 6:
      // The smoothing coefficient is derived per sample from this value, so a
      // plain update is RT-safe and preserves the running gain state.
      config_.gain_smoothing_ms = std::max(0.0f, value);
      break;
    case 7:
      config_.noise_floor_db = value;
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const VocalRiderConfig>(config_));
  return true;
}

void VocalRider::validate_config(const VocalRiderConfig& config) {
  if (config.max_boost_db < 0.0f || config.max_cut_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f || config.gain_smoothing_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid vocal rider configuration");
  }
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

void VocalRider::update_coefficients(const VocalRiderConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
