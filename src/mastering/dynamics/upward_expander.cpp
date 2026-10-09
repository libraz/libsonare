#include "mastering/dynamics/upward_expander.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/linked_gain.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
UpwardExpander::UpwardExpander(UpwardExpanderConfig config) : ConfigBase(std::move(config)) {}

void UpwardExpander::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);

  sample_rate_ = sample_rate;
  prepared_ = true;
  active_ = config_;
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  update_coefficients(active_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void UpwardExpander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "UpwardExpander");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const UpwardExpanderConfig& cfg = *adopt_snapshot_for_block();

  ensure_followers(num_channels);

  // Linked detection (mirrors UpwardCompressor): one detector envelope drives every channel.
  auto& follower = followers_[0];
  const int excluded_channel = detector_excluded_channel(num_channels);
  const float max_gain = apply_linked_gain(
      channels, num_channels, num_samples, excluded_channel, follower,
      [&cfg](float level_db) { return gain_db(level_db, cfg); },
      [](float acc, float applied_db) { return std::max(acc, applied_db); });

  // One float, once per block: the follower is recursive, so a non-finite level
  // that reached it would otherwise outlive every later block. Only followers_[0]
  // advances under linked detection, so it is the only cell to scrub.
  if (follower.discard_if_non_finite()) note_non_finite_discard();

  last_gain_db_ = max_gain;
}

void UpwardExpander::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

bool UpwardExpander::apply_parameter(UpwardExpanderConfig& config, unsigned int param_id,
                                     float value) {
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
      config.range_db = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> UpwardExpander::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

void UpwardExpander::validate_config(const UpwardExpanderConfig& config) {
  if (!(config.ratio >= 1.0f) || config.range_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid upward expander configuration");
  }
  if (!numeric::finite(db_to_linear(config.range_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "upward expander range must produce a finite linear gain");
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
