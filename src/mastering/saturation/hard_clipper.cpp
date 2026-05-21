#include "mastering/saturation/hard_clipper.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mastering::saturation {

HardClipper::HardClipper(HardClipperConfig config) : config_(config) { validate_config(config_); }

void HardClipper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
}

void HardClipper::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("HardClipper must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = std::clamp(channels[ch][i], -config_.ceiling, config_.ceiling);
    }
  }
}

void HardClipper::reset() {}

void HardClipper::set_config(const HardClipperConfig& config) {
  validate_config(config);
  config_ = config;
}

void HardClipper::validate_config(const HardClipperConfig& config) {
  if (!(config.ceiling > 0.0f))
    throw std::invalid_argument("hard clipper ceiling must be positive");
}

}  // namespace sonare::mastering::saturation
