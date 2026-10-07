#include "mastering/dynamics/upward_compressor.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/dynamics/linked_gain.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
UpwardCompressor::UpwardCompressor(UpwardCompressorConfig config) : ConfigBase(std::move(config)) {}

void UpwardCompressor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

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

void UpwardCompressor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "UpwardCompressor");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const UpwardCompressorConfig& cfg = *adopt_snapshot_for_block();

  ensure_followers(num_channels);

  // Linked detection (mirrors Compressor): one detector envelope drives every channel.
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

void UpwardCompressor::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

bool UpwardCompressor::apply_parameter(UpwardCompressorConfig& config, unsigned int param_id,
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

std::vector<rt::ParamDescriptor> UpwardCompressor::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

void UpwardCompressor::validate_config(const UpwardCompressorConfig& config) {
  if (!(config.ratio >= 1.0f) || config.range_db < 0.0f || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid upward compressor configuration");
  }
  if (!numeric::finite(db_to_linear(config.range_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "upward compressor range must produce a finite linear gain");
  }
}

float UpwardCompressor::gain_db(float input_db, const UpwardCompressorConfig& config) {
  if (input_db >= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float below_db = config.threshold_db - input_db;
  const float gain = below_db * (1.0f - 1.0f / config.ratio);
  return std::min(config.range_db, gain);
}

void UpwardCompressor::update_coefficients(const UpwardCompressorConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

void UpwardCompressor::ensure_followers(int num_channels) {
  const auto target_size = static_cast<size_t>(num_channels);
  if (followers_.size() >= target_size) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared UpwardCompressor state");
}

}  // namespace sonare::mastering::dynamics
