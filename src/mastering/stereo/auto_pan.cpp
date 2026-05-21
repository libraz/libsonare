#include "mastering/stereo/auto_pan.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::stereo {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

}  // namespace

AutoPan::AutoPan(AutoPanConfig config) : config_(config) { validate_config(config_); }

void AutoPan::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  reset();
}

void AutoPan::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) {
    throw std::logic_error("AutoPan must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }
  if (num_channels < 2) {
    return;
  }

  const double increment = config_.rate_hz / sample_rate_;
  for (int i = 0; i < num_samples; ++i) {
    const float pan =
        static_cast<float>(std::sin((phase_ + config_.phase) * kTwoPi)) * config_.depth;
    const float left_gain = std::sqrt((1.0f - pan) * 0.5f);
    const float right_gain = std::sqrt((1.0f + pan) * 0.5f);
    channels[0][i] *= left_gain;
    channels[1][i] *= right_gain;
    phase_ += increment;
    phase_ -= std::floor(phase_);
  }
}

void AutoPan::reset() { phase_ = 0.0; }

void AutoPan::set_config(const AutoPanConfig& config) {
  validate_config(config);
  config_ = config;
}

void AutoPan::validate_config(const AutoPanConfig& config) {
  if (config.rate_hz < 0.0f || config.depth < 0.0f || config.depth > 1.0f) {
    throw std::invalid_argument("invalid auto pan configuration");
  }
}

}  // namespace sonare::mastering::stereo
