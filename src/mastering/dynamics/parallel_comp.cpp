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
  active_ = config_;
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
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  if (limiter_gains_.size() < kRealtimePreparedChannels) {
    limiter_gains_.assign(kRealtimePreparedChannels, 1.0f);
  }
  active_ = config_;
  update_coefficients(active_);
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
    // A newly published snapshot (set_config) supersedes any in-place
    // set_parameter automation: copy it into the working config and re-derive.
    active_ = *current;
    update_coefficients(active_);
    applied_snapshot_ = current;
  }
  // The per-sample loop always reads the working config (active_), seeded in the
  // constructor and refreshed above; set_parameter mutates it in place without
  // publishing, so no allocation occurs on the audio thread.
  return &active_;
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
        if (cfg.output_limiter) {
          out = limit_output_sample(out, static_cast<size_t>(ch), ceiling, cfg);
        }
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
        if (cfg.output_limiter) {
          out = limit_output_sample(out, static_cast<size_t>(ch), ceiling, cfg);
        }
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
  std::fill(limiter_gains_.begin(), limiter_gains_.end(), 1.0f);
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
  // RT-safe in-place automation: mutate the audio thread's working config and
  // re-derive coefficients. No shared_ptr publish, no allocation; the published
  // snapshot stays untouched and the control-thread mirror (config_) is updated
  // so config() reads back the automated state. set_parameter and set_config
  // must not run concurrently (single-producer contract).
  switch (param_id) {
    case 0:
      active_.threshold_db = value;
      break;
    case 1:
      active_.ratio = std::max(1.0f, value);
      break;
    case 2:
      active_.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      active_.release_ms = std::max(0.0f, value);
      break;
    case 4:
      active_.makeup_gain_db = value;
      break;
    case 5:
      active_.mix = std::clamp(value, 0.0f, 1.0f);
      break;
    case 6:
      active_.output_ceiling_db = value;
      break;
    default:
      return false;
  }
  update_coefficients(active_);
  config_ = active_;
  return true;
}

std::vector<rt::ParamDescriptor> ParallelComp::parameter_descriptors() const {
  return {{"thresholdDb", 0},  {"ratio", 1}, {"attackMs", 2},       {"releaseMs", 3},
          {"makeupGainDb", 4}, {"mix", 5},   {"outputCeilingDb", 6}};
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

float ParallelComp::limit_output_sample(float sample, size_t channel_index, float ceiling,
                                        const ParallelCompConfig& config) noexcept {
  if (channel_index >= limiter_gains_.size() || !(ceiling > 0.0f)) {
    return sample;
  }
  float& gain = limiter_gains_[channel_index];
  const float magnitude = std::abs(sample);
  if (magnitude > ceiling) {
    gain = std::min(gain, ceiling / magnitude);
  } else if (gain < 1.0f) {
    if (config.release_ms <= 0.0f) {
      gain = 1.0f;
    } else {
      const float coeff =
          std::exp(-1.0f / (0.001f * config.release_ms * static_cast<float>(sample_rate_)));
      gain = 1.0f - (1.0f - gain) * coeff;
      if (gain > 1.0f) gain = 1.0f;
    }
  }
  return sample * gain;
}

void ParallelComp::ensure_followers(int num_channels) {
  // Followers are preallocated to kRealtimePreparedChannels in prepare(); never
  // grow the vector on the audio thread (emplace_back would malloc). Reject
  // blocks wider than the prepared state (matches the established pattern).
  if (followers_.size() >= static_cast<size_t>(num_channels) &&
      limiter_gains_.size() >= static_cast<size_t>(num_channels)) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared ParallelComp state");
}

void ParallelComp::update_coefficients(const ParallelCompConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
