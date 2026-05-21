#include "mastering/dynamics/brickwall_limiter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::dynamics {

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
  if (!prepared_) {
    throw std::logic_error("BrickwallLimiter must be prepared before processing");
  }
  limiter_.process(channels, num_channels, num_samples);

  const float ceiling = db_to_linear(config_.ceiling_db);
  float min_sample_gain = 1.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const float abs_sample = std::abs(channels[ch][i]);
      if (abs_sample > ceiling && abs_sample > 0.0f) {
        const float gain = ceiling / abs_sample;
        channels[ch][i] *= gain;
        min_sample_gain = std::min(min_sample_gain, gain);
      }
    }
  }

  last_gain_reduction_db_ =
      std::min(limiter_.last_gain_reduction_db(), linear_to_db(min_sample_gain));
}

void BrickwallLimiter::reset() {
  limiter_.reset();
  last_gain_reduction_db_ = 0.0f;
}

void BrickwallLimiter::set_config(const BrickwallLimiterConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    prepare(sample_rate_, max_block_size_);
  }
}

void BrickwallLimiter::validate_config(const BrickwallLimiterConfig& config) {
  if (config.lookahead_ms < 0.0f || config.release_ms < 0.0f) {
    throw std::invalid_argument("brickwall limiter timing values must be non-negative");
  }
}

float BrickwallLimiter::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float BrickwallLimiter::linear_to_db(float value) {
  if (value <= 0.0f) {
    return -120.0f;
  }
  return 20.0f * std::log10(value);
}

}  // namespace sonare::mastering::dynamics
