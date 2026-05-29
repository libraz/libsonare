#include "mastering/dynamics/brickwall_limiter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"

namespace sonare::mastering::dynamics {
namespace {

float sanitize_sample(float sample, float ceiling) {
  if (std::isnan(sample)) return 0.0f;
  if (sample == std::numeric_limits<float>::infinity()) return ceiling;
  if (sample == -std::numeric_limits<float>::infinity()) return -ceiling;
  return sample;
}

}  // namespace

BrickwallLimiter::BrickwallLimiter(BrickwallLimiterConfig config)
    : config_(config),
      config_publisher_(std::make_unique<rt::RtPublisher<BrickwallLimiterConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const BrickwallLimiterConfig>(config_));
}

void BrickwallLimiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  // Inner limiter owns the lookahead buffer sizing. lookahead_ms changes
  // require buffer resize and are routed through prepare() (the non-RT-safe
  // control-thread path); the RT-safe snapshot path only forwards ceiling and
  // release updates via update_coefficients().
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const BrickwallLimiterConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const BrickwallLimiterConfig* BrickwallLimiter::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target the inner
  // limiter's threshold/release in place (no resize), so the per-sample loop
  // sees a consistent configuration for the block.
  config_publisher_->acquire();
  const BrickwallLimiterConfig* current = config_publisher_->current();
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

void BrickwallLimiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "BrickwallLimiter");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const BrickwallLimiterConfig& cfg = *adopt_snapshot_for_block();

  limiter_.process(channels, num_channels, num_samples);

  const float ceiling = db_to_linear(cfg.ceiling_db);
  float min_sample_gain = 1.0f;
  hard_clip_count_ = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const float before = channels[ch][i];
      channels[ch][i] = sanitize_sample(channels[ch][i], ceiling);
      const float abs_sample = std::abs(channels[ch][i]);
      if (abs_sample > ceiling && abs_sample > 0.0f) {
        const float gain = ceiling / abs_sample;
        channels[ch][i] *= gain;
        min_sample_gain = std::min(min_sample_gain, gain);
        ++hard_clip_count_;
      } else if (!std::isfinite(before)) {
        min_sample_gain = 0.0f;
        ++hard_clip_count_;
      }
    }
  }

  last_gain_reduction_db_ =
      std::min(limiter_.last_gain_reduction_db(), linear_to_db(min_sample_gain));
}

void BrickwallLimiter::reset() {
  limiter_.reset();
  last_gain_reduction_db_ = 0.0f;
  hard_clip_count_ = 0;
}

void BrickwallLimiter::set_config(const BrickwallLimiterConfig& config) {
  // Control-thread side: validate before mutating any state so any throw
  // leaves both the control-thread mirror (config_) and the audio-thread
  // snapshot unchanged.
  validate_config(config);
  const bool lookahead_changed = prepared_ && config.lookahead_ms != config_.lookahead_ms;
  config_ = config;
  if (lookahead_changed) {
    // Lookahead change resizes the inner limiter's ring buffers — that is NOT
    // RT-safe. Preserved as control-thread behaviour for callers that change
    // lookahead_ms via set_config; this branch MUST NOT race with process().
    // prepare() publishes the snapshot itself, so no extra publish here.
    prepare(sample_rate_, max_block_size_);
    return;
  }
  // RT-safe path: ceiling/release updates propagate through the publisher and
  // are applied on the audio thread via adopt_snapshot_for_block().
  config_publisher_->publish(std::make_shared<const BrickwallLimiterConfig>(config_));
}

void BrickwallLimiter::set_release_ms(float release_ms) {
  if (release_ms < 0.0f) {
    throw std::invalid_argument("brickwall limiter release must be non-negative");
  }
  config_.release_ms = release_ms;
  limiter_.set_release_ms(release_ms);
}

bool BrickwallLimiter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.ceiling_db = value;
      // The inner limiter uses ceiling_db as its threshold; forward it so the
      // soft limiting stage tracks the new ceiling without resetting state.
      // The inner set_parameter publishes its own snapshot internally.
      limiter_.set_parameter(0, value);
      break;
    case 1:
      config_.release_ms = std::max(0.0f, value);
      limiter_.set_release_ms(config_.release_ms);
      break;
    default:
      return false;
  }
  // Publish the mutated config_ as a new snapshot so the audio thread adopts
  // it on the next block. NOT realtime-safe (shared_ptr allocation).
  config_publisher_->publish(std::make_shared<const BrickwallLimiterConfig>(config_));
  return true;
}

void BrickwallLimiter::validate_config(const BrickwallLimiterConfig& config) {
  if (!std::isfinite(config.ceiling_db) || config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("brickwall limiter timing values must be non-negative");
  }
}

void BrickwallLimiter::update_coefficients(const BrickwallLimiterConfig& config) {
  // RT-safe: forward ceiling and release to the inner limiter via its
  // parameter setters, which update scalar coefficients in place without
  // resizing the lookahead buffers.
  limiter_.set_parameter(0, config.ceiling_db);
  limiter_.set_release_ms(config.release_ms);
}

}  // namespace sonare::mastering::dynamics
