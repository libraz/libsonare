#include "mastering/dynamics/brickwall_limiter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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

BrickwallLimiter::BrickwallLimiter(BrickwallLimiterConfig config) : config_(config) {
  validate_config(config_);
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
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
  reset();
}

void BrickwallLimiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("BrickwallLimiter must be prepared before processing");
  }
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

  limiter_.process(channels, num_channels, num_samples);

  const float ceiling = db_to_linear(config_.ceiling_db);
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
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
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
      limiter_.set_parameter(0, value);
      return true;
    case 1:
      config_.release_ms = std::max(0.0f, value);
      limiter_.set_release_ms(config_.release_ms);
      return true;
    default:
      return false;
  }
}

void BrickwallLimiter::validate_config(const BrickwallLimiterConfig& config) {
  if (!std::isfinite(config.ceiling_db) || config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("brickwall limiter timing values must be non-negative");
  }
}

}  // namespace sonare::mastering::dynamics
