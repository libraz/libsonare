#include "mastering/saturation/soft_clipper.h"

#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

SoftClipper::SoftClipper(SoftClipperConfig config) : config_(config) { validate_config(config_); }

void SoftClipper::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
}

void SoftClipper::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("SoftClipper must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  const float drive = Waveshaper::db_to_linear(config_.drive_db);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      const float dry = channels[ch][i];
      const float wet = config_.ceiling * std::tanh(dry * drive / config_.ceiling);
      channels[ch][i] = dry * (1.0f - config_.mix) + wet * config_.mix;
    }
  }
}

void SoftClipper::reset() {}

void SoftClipper::set_config(const SoftClipperConfig& config) {
  validate_config(config);
  config_ = config;
}

void SoftClipper::validate_config(const SoftClipperConfig& config) {
  if (!(config.ceiling > 0.0f) || config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("invalid soft clipper configuration");
  }
}

}  // namespace sonare::mastering::saturation
