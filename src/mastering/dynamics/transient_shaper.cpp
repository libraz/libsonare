#include "mastering/dynamics/transient_shaper.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {
namespace {

constexpr float kEnvelopeFloor = 1.0e-6f;

}  // namespace

TransientShaper::TransientShaper(TransientShaperConfig config)
    : config_(config),
      config_publisher_(std::make_unique<rt::RtPublisher<TransientShaperConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const TransientShaperConfig>(config_));
}

void TransientShaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  const size_t channel_count = kRealtimePreparedChannels;
  fast_followers_.assign(channel_count, {});
  slow_followers_.assign(channel_count, {});
  gain_state_db_.assign(channel_count, 0.0f);
  lookahead_samples_ = static_cast<int>(std::round(sample_rate_ * config_.lookahead_ms * 0.001));
  const size_t lookahead_samples = static_cast<size_t>(std::max(lookahead_samples_, 0));
  lookahead_.assign(channel_count, std::vector<float>(lookahead_samples, 0.0f));
  lookahead_index_.assign(channel_count, 0);
  update_coefficients(config_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const TransientShaperConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const TransientShaperConfig* TransientShaper::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const TransientShaperConfig* current = config_publisher_->current();
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

void TransientShaper::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "TransientShaper");
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
  const TransientShaperConfig& cfg = *adopt_snapshot_for_block();

  ensure_followers(num_channels);
  float largest_abs_gain = 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }

    auto& fast = fast_followers_[static_cast<size_t>(ch)];
    auto& slow = slow_followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float fast_env = fast.process(channels[ch][i]);
      const float slow_env = slow.process(channels[ch][i]);
      const float diff = fast_env - slow_env;
      const float denom = std::max(std::max(fast_env, slow_env), kEnvelopeFloor);
      const float amount = std::clamp(std::abs(diff) / denom * cfg.sensitivity, 0.0f, 1.0f);
      const float target_db = diff >= 0.0f ? cfg.attack_gain_db : cfg.sustain_gain_db;
      const float gain_db = std::clamp(target_db * amount, -cfg.max_gain_db, cfg.max_gain_db);
      auto idx = static_cast<size_t>(ch);
      const float smoothing = gain_smoothing_coeff_;
      gain_state_db_[idx] = smoothing * gain_state_db_[idx] + (1.0f - smoothing) * gain_db;
      float delayed = channels[ch][i];
      if (!lookahead_[idx].empty()) {
        delayed = lookahead_[idx][lookahead_index_[idx]];
        lookahead_[idx][lookahead_index_[idx]] = channels[ch][i];
        lookahead_index_[idx] = (lookahead_index_[idx] + 1) % lookahead_[idx].size();
      }
      channels[ch][i] = delayed * db_to_linear(gain_state_db_[idx]);
      if (std::abs(gain_state_db_[idx]) > std::abs(largest_abs_gain)) {
        largest_abs_gain = gain_state_db_[idx];
      }
    }
  }

  last_gain_db_ = largest_abs_gain;
}

void TransientShaper::reset() {
  for (auto& follower : fast_followers_) {
    follower.reset();
  }
  for (auto& follower : slow_followers_) {
    follower.reset();
  }
  std::fill(gain_state_db_.begin(), gain_state_db_.end(), 0.0f);
  for (auto& delay : lookahead_) std::fill(delay.begin(), delay.end(), 0.0f);
  std::fill(lookahead_index_.begin(), lookahead_index_.end(), 0);
  last_gain_db_ = 0.0f;
}

void TransientShaper::set_config(const TransientShaperConfig& config) {
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const TransientShaperConfig>(config_));
}

bool TransientShaper::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.attack_gain_db = value;
      break;
    case 1:
      config_.sustain_gain_db = value;
      break;
    case 2:
      config_.fast_attack_ms = std::max(0.0f, value);
      break;
    case 3:
      config_.fast_release_ms = std::max(0.0f, value);
      break;
    case 4:
      config_.slow_attack_ms = std::max(0.0f, value);
      break;
    case 5:
      config_.slow_release_ms = std::max(0.0f, value);
      break;
    case 6:
      config_.sensitivity = std::max(0.0f, value);
      break;
    case 7:
      config_.max_gain_db = std::max(0.0f, value);
      break;
    case 8:
      // Recompute the cached smoother coefficient in place; preserves the
      // running gain state. RT-safe (no allocation).
      config_.gain_smoothing_ms = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const TransientShaperConfig>(config_));
  return true;
}

std::vector<rt::ParamDescriptor> TransientShaper::parameter_descriptors() const {
  return {{"attackGainDb", 0},  {"sustainGainDb", 1}, {"fastAttackMs", 2},
          {"fastReleaseMs", 3}, {"slowAttackMs", 4},  {"slowReleaseMs", 5},
          {"sensitivity", 6},   {"maxGainDb", 7},     {"gainSmoothingMs", 8}};
}

void TransientShaper::validate_config(const TransientShaperConfig& config) {
  if (config.fast_attack_ms < 0.0f || config.fast_release_ms < 0.0f ||
      config.slow_attack_ms < 0.0f || config.slow_release_ms < 0.0f || config.sensitivity < 0.0f ||
      config.max_gain_db < 0.0f || config.gain_smoothing_ms < 0.0f || config.lookahead_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid transient shaper configuration");
  }
}

void TransientShaper::ensure_followers(int num_channels) {
  if (static_cast<size_t>(num_channels) > fast_followers_.size() ||
      static_cast<size_t>(num_channels) > slow_followers_.size() ||
      static_cast<size_t>(num_channels) > gain_state_db_.size() ||
      static_cast<size_t>(num_channels) > lookahead_.size() ||
      static_cast<size_t>(num_channels) > lookahead_index_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared TransientShaper state");
  }
}

void TransientShaper::update_coefficients(const TransientShaperConfig& config) {
  gain_smoothing_coeff_ = time_to_coefficient(sample_rate_, config.gain_smoothing_ms);
  for (auto& follower : fast_followers_) {
    follower.prepare(sample_rate_, config.fast_attack_ms, config.fast_release_ms);
  }
  for (auto& follower : slow_followers_) {
    follower.prepare(sample_rate_, config.slow_attack_ms, config.slow_release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
