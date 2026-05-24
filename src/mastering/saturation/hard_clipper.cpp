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
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_sample(channels[ch][i], ch);
    }
  }
}

void HardClipper::reset() {
  for (auto& state : hard_clip_adaa_) state.reset();
  for (auto& state : hard_clip_adaa2_) state.reset();
}

void HardClipper::set_config(const HardClipperConfig& config) {
  validate_config(config);
  const bool reset_state = config_.ceiling != config.ceiling || config_.aliasing != config.aliasing;
  config_ = config;
  if (reset_state) {
    const size_t channels = hard_clip_adaa_.size();
    hard_clip_adaa_.clear();
    hard_clip_adaa_.reserve(channels);
    hard_clip_adaa2_.clear();
    hard_clip_adaa2_.reserve(channels);
    for (size_t i = 0; i < channels; ++i) {
      hard_clip_adaa_.emplace_back(common::HardClipNonlinearity{config_.ceiling});
      hard_clip_adaa2_.emplace_back(common::HardClipNonlinearity{config_.ceiling});
    }
  }
}

void HardClipper::validate_config(const HardClipperConfig& config) {
  if (!(config.ceiling > 0.0f))
    throw std::invalid_argument("hard clipper ceiling must be positive");
}

void HardClipper::ensure_state(int num_channels) {
  if (hard_clip_adaa_.size() != static_cast<size_t>(num_channels)) {
    hard_clip_adaa_.clear();
    hard_clip_adaa_.reserve(static_cast<size_t>(num_channels));
    hard_clip_adaa2_.clear();
    hard_clip_adaa2_.reserve(static_cast<size_t>(num_channels));
    for (int i = 0; i < num_channels; ++i) {
      hard_clip_adaa_.emplace_back(common::HardClipNonlinearity{config_.ceiling});
      hard_clip_adaa2_.emplace_back(common::HardClipNonlinearity{config_.ceiling});
    }
  }
}

int HardClipper::latency_samples() const noexcept {
  return config_.aliasing == common::AliasingControl::Adaa2 ? 1 : 0;
}

float HardClipper::process_sample(float sample, int channel) {
  if (config_.aliasing == common::AliasingControl::Adaa1) {
    return hard_clip_adaa_[static_cast<size_t>(channel)].process(sample);
  }
  if (config_.aliasing == common::AliasingControl::Adaa2) {
    return hard_clip_adaa2_[static_cast<size_t>(channel)].process(sample);
  }
  return std::clamp(sample, -config_.ceiling, config_.ceiling);
}

}  // namespace sonare::mastering::saturation
