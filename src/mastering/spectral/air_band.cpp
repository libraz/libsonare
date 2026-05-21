#include "mastering/spectral/air_band.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::spectral {

AirBand::AirBand(AirBandConfig config) : config_(config) { validate_config(config_); }

void AirBand::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  prepared_ = true;
}

void AirBand::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("AirBand must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    float previous = channels[ch][0];
    for (int i = 1; i < num_samples; ++i) {
      const float high = channels[ch][i] - previous;
      previous = channels[ch][i];
      channels[ch][i] =
          std::clamp(channels[ch][i] + std::tanh(high * 4.0f) * config_.amount, -1.5f, 1.5f);
    }
  }
}

void AirBand::set_config(const AirBandConfig& config) {
  validate_config(config);
  config_ = config;
}

void AirBand::validate_config(const AirBandConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f)) {
    throw std::invalid_argument("invalid air band configuration");
  }
}

}  // namespace sonare::mastering::spectral
