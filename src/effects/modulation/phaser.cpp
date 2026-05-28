#include "effects/modulation/phaser.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::effects::modulation {

using sonare::constants::kPi;

Phaser::Phaser(PhaserConfig config) : config_(config) {}

void Phaser::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const int stages = std::clamp(config_.stages, 1, 12);
  for (int ch = 0; ch < 2; ++ch) {
    x1_[static_cast<size_t>(ch)].assign(static_cast<size_t>(stages), 0.0f);
    y1_[static_cast<size_t>(ch)].assign(static_cast<size_t>(stages), 0.0f);
  }
  lfo_.prepare(sample_rate_);
  lfo_.set_rate_hz(config_.rate_hz);
  reset();
}

void Phaser::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  for (int i = 0; i < num_samples; ++i) {
    const float sweep = 0.5f + 0.5f * lfo_.process();
    const float freq = config_.min_hz + (config_.max_hz - config_.min_hz) * sweep;
    const float t = std::tan(::sonare::constants::kPi * freq / static_cast<float>(sample_rate_));
    const float coeff = (1.0f - t) / (1.0f + t);
    const float in_l = left[i];
    const float in_r = right[i];
    left[i] = dry * in_l + wet * process_channel(in_l, 0, coeff);
    right[i] = dry * in_r + wet * process_channel(in_r, 1, coeff);
  }
}

bool Phaser::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      // Updates the LFO increment in place; preserves oscillator phase.
      lfo_.set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      config_.min_hz = std::clamp(value, 1.0f, static_cast<float>(sample_rate_ * 0.49));
      // Keep the sweep range ordered (min <= max) to avoid inverted/NaN coeffs.
      config_.min_hz = std::min(config_.min_hz, config_.max_hz);
      return true;
    case 2:
      config_.max_hz = std::clamp(value, 1.0f, static_cast<float>(sample_rate_ * 0.49));
      // Keep the sweep range ordered (min <= max) to avoid inverted/NaN coeffs.
      config_.max_hz = std::max(config_.max_hz, config_.min_hz);
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

void Phaser::reset() {
  for (auto& state : x1_) {
    std::fill(state.begin(), state.end(), 0.0f);
  }
  for (auto& state : y1_) {
    std::fill(state.begin(), state.end(), 0.0f);
  }
  lfo_.reset();
}

float Phaser::process_channel(float input, int channel, float coeff) {
  float y = input;
  auto& x = x1_[static_cast<size_t>(channel)];
  auto& z = y1_[static_cast<size_t>(channel)];
  for (size_t stage = 0; stage < x.size(); ++stage) {
    const float out = -coeff * y + x[stage] + coeff * z[stage];
    x[stage] = y;
    z[stage] = out;
    y = out;
  }
  return y;
}

}  // namespace sonare::effects::modulation
