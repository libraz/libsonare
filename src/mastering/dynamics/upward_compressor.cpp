#include "mastering/dynamics/upward_compressor.h"

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

  // Linked detection: derive a single detector envelope from the loudest
  // channel each sample and apply the same gain to every channel. Independent
  // per-channel followers would let the L/R gain diverge on asymmetric content
  // and rotate the stereo image (mirrors Compressor).
  auto& follower = followers_[0];
  float max_gain = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float linked_level = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      linked_level = std::max(linked_level, std::abs(channels[ch][i]));
    }
    const float envelope = follower.process(linked_level);
    const float applied_gain_db = gain_db(linear_to_db(envelope), cfg);
    const float gain = db_to_linear(applied_gain_db);
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] *= gain;
    }
    max_gain = std::max(max_gain, applied_gain_db);
  }

  last_gain_db_ = max_gain;
}

void UpwardCompressor::reset() {
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_db_ = 0.0f;
}

bool UpwardCompressor::set_parameter(unsigned int param_id, float value) {
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
      active_.range_db = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  update_coefficients(active_);
  config_ = active_;
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
