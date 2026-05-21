#include "mastering/maximizer/adaptive_release.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::maximizer {

AdaptiveRelease::AdaptiveRelease(AdaptiveReleaseConfig config) : config_(config) {
  validate_config(config_);
}

void AdaptiveRelease::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  current_release_ms_ = config_.min_release_ms;
  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void AdaptiveRelease::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("AdaptiveRelease must be prepared before processing");
  const float previous_reduction = std::abs(limiter_.last_gain_reduction_db());
  const float amount = std::clamp(previous_reduction / 12.0f, 0.0f, 1.0f);
  current_release_ms_ =
      config_.min_release_ms + (config_.max_release_ms - config_.min_release_ms) * amount;
  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  limiter_.process(channels, num_channels, num_samples);
}

void AdaptiveRelease::reset() { limiter_.reset(); }

void AdaptiveRelease::set_config(const AdaptiveReleaseConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

void AdaptiveRelease::validate_config(const AdaptiveReleaseConfig& config) {
  if (config.lookahead_ms < 0.0f || config.min_release_ms < 0.0f ||
      config.max_release_ms < config.min_release_ms) {
    throw std::invalid_argument("invalid adaptive release configuration");
  }
}

void AdaptiveRelease::configure_limiter() {
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, current_release_ms_, 4});
}

}  // namespace sonare::mastering::maximizer
