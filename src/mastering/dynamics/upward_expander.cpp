#include "mastering/dynamics/upward_expander.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

UpwardExpander::UpwardExpander(UpwardExpanderConfig config)
    : config_(config),
      config_publisher_(std::make_unique<rt::RtPublisher<UpwardExpanderConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const UpwardExpanderConfig>(config_));
}

void UpwardExpander::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  prepared_ = true;
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  update_coefficients(config_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const UpwardExpanderConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const UpwardExpanderConfig* UpwardExpander::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const UpwardExpanderConfig* current = config_publisher_->current();
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

void UpwardExpander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "UpwardExpander");
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

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const UpwardExpanderConfig& cfg = *adopt_snapshot_for_block();

  ensure_followers(num_channels);
  float max_gain = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }

    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float envelope = follower.process(channels[ch][i]);
      const float applied_gain_db = gain_db(linear_to_db(envelope), cfg);
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
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const UpwardExpanderConfig>(config_));
}

bool UpwardExpander::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      break;
    case 1:
      config_.ratio = std::max(1.0f, value);
      break;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      break;
    case 4:
      config_.range_db = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const UpwardExpanderConfig>(config_));
  return true;
}

void UpwardExpander::validate_config(const UpwardExpanderConfig& config) {
  if (!(config.ratio >= 1.0f) || config.range_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid upward expander configuration");
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

void UpwardExpander::update_coefficients(const UpwardExpanderConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

void UpwardExpander::ensure_followers(int num_channels) {
  const auto target_size = static_cast<size_t>(num_channels);
  if (followers_.size() >= target_size) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared UpwardExpander state");
}

}  // namespace sonare::mastering::dynamics
