#include "mastering/spectral/spectral_shaper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::spectral {

SpectralShaper::SpectralShaper(SpectralShaperConfig config) : config_(config) {
  validate_config(config_);
}

void SpectralShaper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void SpectralShaper::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("SpectralShaper must be prepared before processing");
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
      static_cast<float>(2.0 * 3.14159265358979323846 * config_.frequency_hz /
                         (2.0 * 3.14159265358979323846 * config_.frequency_hz + sample_rate_)),
      0.0f, 1.0f);
  float min_gain = 1.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    float low = low_state_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      low += alpha * (channels[ch][i] - low);
      const float high = channels[ch][i] - low;
      const float level = std::abs(high);
      float gain = 1.0f;
      if (level > config_.threshold) {
        const float over = (level - config_.threshold) / std::max(level, 1e-6f);
        gain = std::clamp(1.0f - over * config_.amount, 0.0f, 1.0f);
        min_gain = std::min(min_gain, gain);
      }
      channels[ch][i] = low + high * gain;
    }
    low_state_[static_cast<size_t>(ch)] = low;
  }
  last_reduction_db_ = min_gain <= 0.0f ? -120.0f : 20.0f * std::log10(min_gain);
}

void SpectralShaper::reset() {
  std::fill(low_state_.begin(), low_state_.end(), 0.0f);
  last_reduction_db_ = 0.0f;
}

void SpectralShaper::set_config(const SpectralShaperConfig& config) {
  validate_config(config);
  config_ = config;
}

void SpectralShaper::validate_config(const SpectralShaperConfig& config) {
  if (!(config.threshold >= 0.0f) || !(config.amount >= 0.0f && config.amount <= 1.0f) ||
      !(config.frequency_hz > 0.0f)) {
    throw std::invalid_argument("invalid spectral shaper configuration");
  }
}

}  // namespace sonare::mastering::spectral
