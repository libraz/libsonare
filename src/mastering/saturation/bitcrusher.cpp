#include "mastering/saturation/bitcrusher.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

BitCrusher::BitCrusher(BitCrusherConfig config) : config_(config) { validate_config(config_); }

void BitCrusher::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
  reset();
}

void BitCrusher::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("BitCrusher must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      if (counters_[static_cast<size_t>(ch)] == 0) {
        held_[static_cast<size_t>(ch)] = quantize(channels[ch][i], config_.bit_depth);
      }
      counters_[static_cast<size_t>(ch)] =
          (counters_[static_cast<size_t>(ch)] + 1) % config_.downsample_factor;
      channels[ch][i] =
          channels[ch][i] * (1.0f - config_.mix) + held_[static_cast<size_t>(ch)] * config_.mix;
    }
  }
}

void BitCrusher::reset() {
  std::fill(held_.begin(), held_.end(), 0.0f);
  std::fill(counters_.begin(), counters_.end(), 0);
}

void BitCrusher::set_config(const BitCrusherConfig& config) {
  validate_config(config);
  config_ = config;
}

void BitCrusher::validate_config(const BitCrusherConfig& config) {
  if (config.bit_depth < 1 || config.bit_depth > 24 || config.downsample_factor < 1 ||
      config.mix < 0.0f || config.mix > 1.0f) {
    throw std::invalid_argument("invalid bitcrusher configuration");
  }
}

float BitCrusher::quantize(float sample, int bit_depth) {
  const float levels = std::pow(2.0f, static_cast<float>(bit_depth - 1)) - 1.0f;
  return std::round(std::clamp(sample, -1.0f, 1.0f) * levels) / levels;
}

void BitCrusher::ensure_state(int num_channels) {
  if (held_.size() != static_cast<size_t>(num_channels)) {
    held_.assign(static_cast<size_t>(num_channels), 0.0f);
    counters_.assign(static_cast<size_t>(num_channels), 0);
  }
}

}  // namespace sonare::mastering::saturation
