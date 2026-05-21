#include "mastering/spectral/low_end_focus.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::spectral {

LowEndFocus::LowEndFocus(LowEndFocusConfig config) : config_(config) { validate_config(config_); }

void LowEndFocus::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void LowEndFocus::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("LowEndFocus must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  if (low_state_.size() != static_cast<size_t>(num_channels)) {
    low_state_.assign(static_cast<size_t>(num_channels), 0.0f);
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
  }

  const float alpha = std::clamp(
      static_cast<float>(2.0 * 3.14159265358979323846 * config_.cutoff_hz /
                         (2.0 * 3.14159265358979323846 * config_.cutoff_hz + sample_rate_)),
      0.0f, 1.0f);
  for (int i = 0; i < num_samples; ++i) {
    for (int ch = 0; ch < num_channels; ++ch) {
      low_state_[static_cast<size_t>(ch)] +=
          alpha * (channels[ch][i] - low_state_[static_cast<size_t>(ch)]);
    }
    if (num_channels >= 2) {
      const float mono_low = 0.5f * (low_state_[0] + low_state_[1]);
      for (int ch = 0; ch < 2; ++ch) {
        const float low =
            mono_low + (low_state_[static_cast<size_t>(ch)] - mono_low) * config_.width;
        const float high = channels[ch][i] - low_state_[static_cast<size_t>(ch)];
        channels[ch][i] = high + low;
      }
    }
  }
}

void LowEndFocus::reset() { std::fill(low_state_.begin(), low_state_.end(), 0.0f); }

void LowEndFocus::set_config(const LowEndFocusConfig& config) {
  validate_config(config);
  config_ = config;
}

void LowEndFocus::validate_config(const LowEndFocusConfig& config) {
  if (!(config.cutoff_hz > 0.0f) || !(config.width >= 0.0f && config.width <= 2.0f)) {
    throw std::invalid_argument("invalid low end focus configuration");
  }
}

}  // namespace sonare::mastering::spectral
