#include "mastering/spectral/presence_enhancer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::constants::kPiD;

PresenceEnhancer::Biquad make_bandpass(double frequency_hz, double sample_rate, double q) {
  const double w0 = 2.0 * kPiD * std::clamp(frequency_hz, 20.0, sample_rate * 0.49) / sample_rate;
  const double c = std::cos(w0);
  const double s = std::sin(w0);
  const double alpha = s / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double inv = 1.0 / a0;
  PresenceEnhancer::Biquad b;
  b.b0 = static_cast<float>(alpha * inv);
  b.b1 = 0.0f;
  b.b2 = static_cast<float>(-alpha * inv);
  b.a1 = static_cast<float>(-2.0 * c * inv);
  b.a2 = static_cast<float>((1.0 - alpha) * inv);
  return b;
}

}  // namespace

PresenceEnhancer::PresenceEnhancer(PresenceEnhancerConfig config) : config_(config) {
  validate_config(config_);
}

void PresenceEnhancer::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw std::invalid_argument("invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  bandpass_.clear();
}

void PresenceEnhancer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) throw std::logic_error("PresenceEnhancer must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    for (int i = 0; i < num_samples; ++i) {
      const float presence = bandpass_[static_cast<size_t>(ch)].process(channels[ch][i]);
      const float harmonic = std::tanh(presence * config_.drive);
      channels[ch][i] = std::clamp(channels[ch][i] + harmonic * config_.amount, -1.5f, 1.5f);
    }
  }
}

void PresenceEnhancer::set_config(const PresenceEnhancerConfig& config) {
  validate_config(config);
  config_ = config;
  bandpass_.clear();
}

void PresenceEnhancer::reset() {
  for (auto& filter : bandpass_) filter.reset();
}

bool PresenceEnhancer::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 1:
      config_.drive = std::max(value, 1.0e-6f);
      return true;
    case 2:
    case 3: {
      if (param_id == 2) {
        config_.center_frequency_hz = std::max(value, 1.0e-3f);
      } else {
        config_.q = std::max(value, 1.0e-6f);
      }
      // Recompute the cached bandpass coefficients in place, preserving each
      // channel's filter state (z1/z2).
      for (auto& filter : bandpass_) {
        const Biquad updated = make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q);
        filter.b0 = updated.b0;
        filter.b1 = updated.b1;
        filter.b2 = updated.b2;
        filter.a1 = updated.a1;
        filter.a2 = updated.a2;
      }
      return true;
    }
    default:
      return false;
  }
}

void PresenceEnhancer::validate_config(const PresenceEnhancerConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f) || !(config.drive > 0.0f) ||
      !(config.center_frequency_hz > 0.0f) || !(config.q > 0.0f)) {
    throw std::invalid_argument("invalid presence enhancer configuration");
  }
}

void PresenceEnhancer::ensure_state(int num_channels) {
  if (bandpass_.size() != static_cast<size_t>(num_channels)) {
    bandpass_.assign(static_cast<size_t>(num_channels),
                     make_bandpass(config_.center_frequency_hz, sample_rate_, config_.q));
  }
}

float PresenceEnhancer::Biquad::process(float x) {
  const float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void PresenceEnhancer::Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

}  // namespace sonare::mastering::spectral
