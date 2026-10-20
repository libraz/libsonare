#include "mastering/dynamics/parallel_comp.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/detector_level.h"
#include "mastering/dynamics/linked_gain.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::dynamics {

namespace {

// Field domains shared by validate_config and live automation.
bool valid_ratio(float v) noexcept { return std::isfinite(v) && v >= 1.0f; }
bool valid_time_ms(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }
bool valid_mix(float v) noexcept { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }
bool valid_makeup_db(float v) noexcept {
  return std::isfinite(v) && numeric::finite(db_to_linear(v));
}
bool valid_ceiling_db(float v) noexcept {
  if (!std::isfinite(v)) return false;
  const float linear = db_to_linear(v);
  return numeric::finite(linear) && linear > 0.0f;
}

float update_limiter_sample(float sample, float& gain, float ceiling,
                            const ParallelCompConfig& config, float release_coeff) noexcept {
  if (!(ceiling > 0.0f)) return sample;
  const float magnitude = std::abs(sample);
  // Attack is instant; recovery toward the gain this sample needs follows the
  // release whether or not the sample is still over the ceiling.
  const float target = magnitude > ceiling ? ceiling / magnitude : 1.0f;
  if (target <= gain) {
    gain = target;
  } else if (config.release_ms <= 0.0f) {
    gain = target;
  } else {
    gain = target - (target - gain) * release_coeff;
    if (gain > target) gain = target;
  }
  return sample * gain;
}

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
ParallelComp::ParallelComp(ParallelCompConfig config) : ConfigBase(std::move(config)) {}

void ParallelComp::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);

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
  const bool output_limiter = output_limiter_gate_.admit(cfg.output_limiter, [this] {
    std::fill(limiter_gains_.begin(), limiter_gains_.end(), 1.0f);
  });

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

  const float ceiling = db_to_linear(cfg.output_ceiling_db);
  float max_reduction = 0.0f;
  bool discarded = false;
  if (cfg.linked_detection) {
    const int excluded_channel = detector_excluded_channel(num_channels);
    for (int i = 0; i < num_samples; ++i) {
      const float linked_level = linked_peak(channels, num_channels, i, excluded_channel);
      const float level = followers_[0].process(linked_level);
      const float reduction_db = gain_reduction_db(detector_level_db(level), cfg);
      const float gain = db_to_linear(reduction_db + cfg.makeup_gain_db);
      float output_peak = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) {
        const float dry = channels[ch][i];
        const float compressed = dry * gain;
        const float out = dry * (1.0f - cfg.mix) + compressed * cfg.mix;
        output_peak = std::max(output_peak, std::abs(out));
        channels[ch][i] = out;
      }
      if (output_limiter) {
        // Protect every output plane with the same gain, including planes
        // excluded from the compressor detector.
        update_limiter_sample(output_peak, limiter_gains_[0], ceiling, cfg, limiter_release_coeff_);
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
        const float reduction_db = gain_reduction_db(detector_level_db(level), cfg);
        const float compressed = dry * db_to_linear(reduction_db + cfg.makeup_gain_db);
        float out = dry * (1.0f - cfg.mix) + compressed * cfg.mix;
        if (output_limiter) {
          out = update_limiter_sample(out, limiter_gains_[static_cast<size_t>(ch)], ceiling, cfg,
                                      limiter_release_coeff_);
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
  output_limiter_gate_.close();
  last_linked_detection_ = active_.linked_detection;
  last_gain_reduction_db_ = 0.0f;
}

bool ParallelComp::apply_parameter(ParallelCompConfig& config, unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config.threshold_db = value;
      break;
    case 1:
      if (!valid_ratio(value)) return false;
      config.ratio = value;
      break;
    case 2:
      if (!valid_time_ms(value)) return false;
      config.attack_ms = value;
      break;
    case 3:
      if (!valid_time_ms(value)) return false;
      config.release_ms = value;
      break;
    case 4:
      if (!valid_makeup_db(value)) return false;
      config.makeup_gain_db = value;
      break;
    case 5:
      if (!valid_mix(value)) return false;
      config.mix = value;
      break;
    case 6:
      if (!valid_ceiling_db(value)) return false;
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
      !valid_ratio(config.ratio) || !valid_time_ms(config.attack_ms) ||
      !valid_time_ms(config.release_ms) || !valid_mix(config.mix) ||
      !std::isfinite(config.output_ceiling_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid parallel compressor configuration");
  }
  if (!valid_ceiling_db(config.output_ceiling_db)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "parallel compressor ceiling must produce a finite positive linear gain");
  }
  if (!valid_makeup_db(config.makeup_gain_db)) {
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

void ParallelComp::ensure_followers(int num_channels) {
  // Followers are preallocated to kRealtimePreparedChannels in prepare(); never
  // grow the vector on the audio thread (emplace_back would malloc). Reject
  // blocks wider than the prepared state (matches the established pattern).
  require_prepared_channels(num_channels, std::min(followers_.size(), limiter_gains_.size()),
                            "ParallelComp");
}

void ParallelComp::update_coefficients(const ParallelCompConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
  // Float evaluation, not time_to_coefficient's double: keeps the limiter release bit-identical.
  limiter_release_coeff_ =
      std::exp(-1.0f / (0.001f * config.release_ms * static_cast<float>(sample_rate_)));
}

}  // namespace sonare::mastering::dynamics
