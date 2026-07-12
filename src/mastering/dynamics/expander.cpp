#include "mastering/dynamics/expander.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
Expander::Expander(ExpanderConfig config) : ConfigBase(std::move(config)) {}

void Expander::prepare(double sample_rate, int max_block_size) {
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

void Expander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Expander");
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
  const ExpanderConfig& cfg = *adopt_snapshot_for_block();

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

  // Linked detection: derive a single detector envelope from the loudest
  // channel each sample and apply the same gain to every channel. Independent
  // per-channel followers would let the L/R gain reduction diverge on
  // asymmetric content and rotate the stereo image (mirrors Compressor).
  auto& follower = followers_[0];
  float min_reduction = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float linked_level = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      linked_level = std::max(linked_level, std::abs(channels[ch][i]));
    }
    const float envelope = follower.process(linked_level);
    const float reduction_db = gain_reduction_db(linear_to_db(envelope), cfg);
    const float gain = db_to_linear(reduction_db);
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] *= gain;
    }
    min_reduction = std::min(min_reduction, reduction_db);
  }

  last_gain_reduction_db_ = min_reduction;
}

void Expander::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

bool Expander::set_parameter(unsigned int param_id, float value) {
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
      active_.range_db = std::min(0.0f, value);
      break;
    default:
      return false;
  }
  update_coefficients(active_);
  config_ = active_;
  return true;
}

std::vector<rt::ParamDescriptor> Expander::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"ratio", 1}, {"attackMs", 2}, {"releaseMs", 3}, {"rangeDb", 4}};
}

void Expander::validate_config(const ExpanderConfig& config) {
  if (!(config.ratio >= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "expander ratio must be at least 1");
  }
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f) {
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

void Expander::ensure_followers(int num_channels) {
  if (followers_.size() >= static_cast<size_t>(num_channels)) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared Expander state");
}

void Expander::update_coefficients(const ExpanderConfig& config) {
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
