#include "mastering/dynamics/parallel_comp.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
ParallelComp::ParallelComp(ParallelCompConfig config) : ConfigBase(std::move(config)) {}

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
  republish_after_prepare();
}

void ParallelComp::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "ParallelComp");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  ensure_followers(num_channels);

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const ParallelCompConfig& cfg = *adopt_snapshot_for_block();

  if (cfg.linked_detection != last_linked_detection_) {
    if (cfg.linked_detection) {
      float envelope = 0.0f;
      float gain = 1.0f;
      const int excluded_channel = detector_excluded_channel(num_channels);
      for (int ch = 0; ch < num_channels; ++ch) {
        if (ch != excluded_channel) envelope = std::max(envelope, followers_[ch].value());
        gain = std::min(gain, limiter_gains_[ch]);
      }
      followers_[0].reset(envelope);
      limiter_gains_[0] = gain;
    } else {
      const float envelope = followers_[0].value();
      for (auto& follower : followers_) follower.reset(envelope);
      std::fill(limiter_gains_.begin(), limiter_gains_.end(), limiter_gains_[0]);
    }
    last_linked_detection_ = cfg.linked_detection;
  }

  float max_reduction = 0.0f;
  const float ceiling = db_to_linear(cfg.output_ceiling_db);
  bool discarded = false;
  if (cfg.linked_detection) {
    const int excluded_channel = detector_excluded_channel(num_channels);
    for (int i = 0; i < num_samples; ++i) {
      float linked_level = 0.0f;
      if (excluded_channel < 0) {
        for (int ch = 0; ch < num_channels; ++ch) {
          linked_level = std::max(linked_level, std::abs(channels[ch][i]));
        }
      } else {
        for (int ch = 0; ch < num_channels; ++ch) {
          if (ch == excluded_channel) continue;
          linked_level = std::max(linked_level, std::abs(channels[ch][i]));
        }
      }
      const float level = followers_[0].process(linked_level);
      const float reduction_db = gain_reduction_db(linear_to_db(level), cfg);
      float output_peak = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        const float dry = channels[ch][i];
        const float compressed = dry * db_to_linear(reduction_db + cfg.makeup_gain_db);
        const float out = dry * (1.0f - cfg.mix) + compressed * cfg.mix;
        output_peak = std::max(output_peak, std::abs(out));
        channels[ch][i] = out;
      }
      if (cfg.output_limiter) {
        // Protect every output plane with the same gain, including planes
        // excluded from the compressor detector.
        limit_output_sample(output_peak, 0, ceiling, cfg);
        for (int ch = 0; ch < num_channels; ++ch) {
          channels[ch][i] *= limiter_gains_[0];
        }
      }
      max_reduction = std::min(max_reduction, reduction_db);
    }
    // One float, once per block: the follower is recursive, so a non-finite
    // level that reached it would otherwise outlive every later block. The
    // limiter gain is recursive too but cannot be stranded -- a non-finite
    // magnitude fails the ordered comparison that is the only path into it.
    discarded |= followers_[0].discard_if_non_finite();
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
      // One float per channel, once per block; see the linked branch.
      discarded |= follower.discard_if_non_finite();
    }
  }
  if (discarded) note_non_finite_discard();

  last_gain_reduction_db_ = max_reduction;
}

void ParallelComp::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  std::fill(limiter_gains_.begin(), limiter_gains_.end(), 1.0f);
  last_linked_detection_ = active_.linked_detection;
  last_gain_reduction_db_ = 0.0f;
}

bool ParallelComp::apply_parameter(ParallelCompConfig& config, unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config.threshold_db = value;
      break;
    case 1:
      config.ratio = std::max(1.0f, value);
      break;
    case 2:
      config.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      config.release_ms = std::max(0.0f, value);
      break;
    case 4:
      if (!numeric::finite(db_to_linear(value))) return false;
      config.makeup_gain_db = value;
      break;
    case 5:
      config.mix = std::clamp(value, 0.0f, 1.0f);
      break;
    case 6:
      if (!numeric::finite(db_to_linear(value)) || !(db_to_linear(value) > 0.0f)) return false;
      config.output_ceiling_db = value;
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> ParallelComp::parameter_descriptors() const {
  return {{"thresholdDb", 0},  {"ratio", 1}, {"attackMs", 2},       {"releaseMs", 3},
          {"makeupGainDb", 4}, {"mix", 5},   {"outputCeilingDb", 6}};
}

void ParallelComp::validate_config(const ParallelCompConfig& config) {
  if (!std::isfinite(config.threshold_db) || !std::isfinite(config.ratio) ||
      !std::isfinite(config.attack_ms) || !std::isfinite(config.release_ms) ||
      !std::isfinite(config.mix) || !std::isfinite(config.makeup_gain_db) ||
      !(config.ratio >= 1.0f) || config.attack_ms < 0.0f || config.release_ms < 0.0f ||
      config.mix < 0.0f || config.mix > 1.0f || !std::isfinite(config.output_ceiling_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid parallel compressor configuration");
  }
  const float ceiling = db_to_linear(config.output_ceiling_db);
  if (!numeric::finite(ceiling) || !(ceiling > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "parallel compressor ceiling must produce a finite positive linear gain");
  }
  if (!numeric::finite(db_to_linear(config.makeup_gain_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "parallel compressor makeup gain must produce a finite linear gain");
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
  // Attack is instant; recovery toward the gain this sample needs follows the
  // release whether or not the sample is still over the ceiling.
  const float target = magnitude > ceiling ? ceiling / magnitude : 1.0f;
  if (target <= gain) {
    gain = target;
  } else if (config.release_ms <= 0.0f) {
    gain = target;
  } else {
    const float coeff =
        std::exp(-1.0f / (0.001f * config.release_ms * static_cast<float>(sample_rate_)));
    gain = target - (target - gain) * coeff;
    if (gain > target) gain = target;
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
