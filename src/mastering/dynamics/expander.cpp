#include "mastering/dynamics/expander.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/linked_gain.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

namespace {

// Field domains shared by validate_config and live automation.
bool valid_ratio(float v) noexcept { return std::isfinite(v) && v >= 1.0f; }
bool valid_time_ms(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }
bool valid_range_db(float v) noexcept { return std::isfinite(v) && v <= 0.0f; }

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
Expander::Expander(ExpanderConfig config) : ConfigBase(std::move(config)) {}

void Expander::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);

  sample_rate_ = sample_rate;
  prepared_ = true;
  active_ = config_;
  update_coefficients(active_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void Expander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Expander");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  require_prepared_channels(num_channels, kRealtimePreparedChannels, "Expander");

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const ExpanderConfig& cfg = *adopt_snapshot_for_block();

  // Linked detection (mirrors Compressor): one detector envelope drives every channel.
  auto& follower = follower_;
  const int excluded_channel = detector_excluded_channel(num_channels);
  const float min_reduction = apply_linked_gain(
      channels, num_channels, num_samples, excluded_channel, follower,
      [&cfg](float level_db) { return gain_reduction_db(level_db, cfg); },
      [](float acc, float applied_db) { return std::min(acc, applied_db); });

  // One float, once per block: the follower is recursive, so a non-finite level
  // that reached it would otherwise outlive every later block. The
  // single follower drives every channel, so it is the only cell to scrub.
  if (follower.discard_if_non_finite()) note_non_finite_discard();

  last_gain_reduction_db_ = min_reduction;
}

void Expander::reset() {
  follower_.reset();
  last_gain_reduction_db_ = 0.0f;
}

bool Expander::apply_parameter(ExpanderConfig& config, unsigned int param_id, float value) {
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
      if (!valid_range_db(value)) return false;
      config.range_db = value;
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> Expander::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

void Expander::validate_config(const ExpanderConfig& config) {
  if (!std::isfinite(config.threshold_db) || !std::isfinite(config.ratio) ||
      !std::isfinite(config.attack_ms) || !std::isfinite(config.release_ms) ||
      !std::isfinite(config.range_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid expander configuration");
  }
  if (!valid_ratio(config.ratio)) {
    throw SonareException(ErrorCode::InvalidParameter, "expander ratio must be at least 1");
  }
  if (!valid_time_ms(config.attack_ms) || !valid_time_ms(config.release_ms) ||
      !valid_range_db(config.range_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid expander configuration");
  }
}

float Expander::gain_reduction_db(float input_db, const ExpanderConfig& config) {
  if (input_db >= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float below_db = config.threshold_db - input_db;
  const float reduction = below_db * (config.ratio - 1.0f);
  return std::max(config.range_db, -reduction);
}

void Expander::update_coefficients(const ExpanderConfig& config) {
  follower_.prepare(sample_rate_, config.attack_ms, config.release_ms);
}

}  // namespace sonare::mastering::dynamics
