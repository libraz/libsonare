#include "mastering/dynamics/parallel_comp.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

ParallelComp::ParallelComp(ParallelCompConfig config)
    : config_(config), config_publisher_(std::make_unique<rt::RtPublisher<ParallelCompConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const ParallelCompConfig>(config_));
}

void ParallelComp::prepare(double sample_rate, int max_block_size) {
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
  auto fresh = std::make_shared<const ParallelCompConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const ParallelCompConfig* ParallelComp::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const ParallelCompConfig* current = config_publisher_->current();
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

void ParallelComp::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "ParallelComp");
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
  const ParallelCompConfig& cfg = *adopt_snapshot_for_block();

  float max_reduction = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }
  const float ceiling = db_to_linear(cfg.output_ceiling_db);
  if (cfg.linked_detection) {
    for (int i = 0; i < num_samples; ++i) {
      float linked_level = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        linked_level = std::max(linked_level, std::abs(channels[ch][i]));
      }
      const float level = followers_[0].process(linked_level);
      const float reduction_db = gain_reduction_db(linear_to_db(level), cfg);
      for (int ch = 0; ch < num_channels; ++ch) {
        const float dry = channels[ch][i];
        const float compressed = dry * db_to_linear(reduction_db + cfg.makeup_gain_db);
        float out = dry * (1.0f - cfg.mix) + compressed * cfg.mix;
        if (cfg.output_limiter && std::abs(out) > ceiling) out = std::copysign(ceiling, out);
        channels[ch][i] = out;
      }
      max_reduction = std::min(max_reduction, reduction_db);
    }
  } else {
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& follower = followers_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        const float dry = channels[ch][i];
        const float level = follower.process(std::abs(dry));
        const float reduction_db = gain_reduction_db(linear_to_db(level), cfg);
        const float compressed = dry * db_to_linear(reduction_db + cfg.makeup_gain_db);
        float out = dry * (1.0f - cfg.mix) + compressed * cfg.mix;
        if (cfg.output_limiter && std::abs(out) > ceiling) out = std::copysign(ceiling, out);
        channels[ch][i] = out;
        max_reduction = std::min(max_reduction, reduction_db);
      }
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void ParallelComp::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

void ParallelComp::set_config(const ParallelCompConfig& config) {
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const ParallelCompConfig>(config_));
}

bool ParallelComp::set_parameter(unsigned int param_id, float value) {
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
      config_.makeup_gain_db = value;
      break;
    case 5:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      break;
    case 6:
      config_.output_ceiling_db = value;
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const ParallelCompConfig>(config_));
  return true;
}

void ParallelComp::validate_config(const ParallelCompConfig& config) {
  if (!(config.ratio >= 1.0f) || config.attack_ms < 0.0f || config.release_ms < 0.0f ||
      config.mix < 0.0f || config.mix > 1.0f || !std::isfinite(config.output_ceiling_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid parallel compressor configuration");
  }
}

float ParallelComp::gain_reduction_db(float input_db, const ParallelCompConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  return -over_db * (1.0f - 1.0f / config.ratio);
}

void ParallelComp::ensure_followers(int num_channels) {
  if (followers_.size() == static_cast<size_t>(num_channels)) {
    return;
  }

  followers_.assign(static_cast<size_t>(num_channels), {});
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  }
}

void ParallelComp::update_coefficients(const ParallelCompConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
