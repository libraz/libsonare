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
  current_crest_factor_ = 0.0f;
  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  prepared_ = true;
}

void AdaptiveRelease::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("AdaptiveRelease must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    limiter_.process(channels, num_channels, num_samples);
    return;
  }
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");

  current_crest_factor_ = compute_crest_factor(channels, num_channels, num_samples);

  // Map crest factor onto [0, 1] then onto [max_release, min_release]:
  // high crest (transient) -> short release, low crest (sustained) -> long release.
  const float crest_span = std::max(0.0001f, config_.crest_high - config_.crest_low);
  const float norm =
      std::clamp((current_crest_factor_ - config_.crest_low) / crest_span, 0.0f, 1.0f);
  current_release_ms_ =
      config_.min_release_ms + (config_.max_release_ms - config_.min_release_ms) * (1.0f - norm);

  configure_limiter();
  limiter_.prepare(sample_rate_, max_block_size_);
  limiter_.process(channels, num_channels, num_samples);
}

void AdaptiveRelease::reset() {
  limiter_.reset();
  current_release_ms_ = config_.min_release_ms;
  current_crest_factor_ = 0.0f;
}

void AdaptiveRelease::set_config(const AdaptiveReleaseConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) prepare(sample_rate_, max_block_size_);
}

void AdaptiveRelease::validate_config(const AdaptiveReleaseConfig& config) {
  if (config.lookahead_ms < 0.0f || config.min_release_ms < 0.0f ||
      config.max_release_ms < config.min_release_ms || config.crest_window_ms <= 0.0f ||
      config.crest_low <= 0.0f || config.crest_high <= config.crest_low) {
    throw std::invalid_argument("invalid adaptive release configuration");
  }
}

void AdaptiveRelease::configure_limiter() {
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, current_release_ms_, 4});
}

float AdaptiveRelease::compute_crest_factor(float* const* channels, int num_channels,
                                            int num_samples) const {
  // Use the most recent crest_window_ms slice of the buffer (or the whole block
  // if it is shorter than the window).
  const int window_samples =
      std::max(1, static_cast<int>(sample_rate_ * config_.crest_window_ms * 0.001));
  const int start = std::max(0, num_samples - window_samples);
  const int length = num_samples - start;
  if (length <= 0) return 0.0f;

  float peak = 0.0f;
  double sum_sq = 0.0;
  long count = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) continue;
    for (int i = start; i < num_samples; ++i) {
      const float s = channels[ch][i];
      peak = std::max(peak, std::abs(s));
      sum_sq += static_cast<double>(s) * static_cast<double>(s);
      ++count;
    }
  }
  if (count == 0) return 0.0f;
  const double rms = std::sqrt(sum_sq / static_cast<double>(count));
  if (rms < 1e-9) return 0.0f;
  return static_cast<float>(peak / rms);
}

}  // namespace sonare::mastering::maximizer
