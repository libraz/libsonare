#include "effects/modulation/chorus.h"

#include <algorithm>

namespace sonare::effects::modulation {

namespace {
// Minimum delay-buffer length so the buffer is never smaller than a typical
// chorus range even for tiny configured delays.
constexpr float kMinDelayBufferSeconds = 0.1f;  // 100 ms
}  // namespace

Chorus::Chorus(ChorusConfig config) : config_(config) {}

void Chorus::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Size the buffer from the actual maximum modulated delay (center + full LFO
  // depth) so the configured modulation is not silently clipped, with a 100 ms
  // floor. The delay line additionally clamps reads to this length at runtime,
  // so later parameter automation can never cause an out-of-bounds read.
  const float max_delay_ms =
      std::max(0.0f, config_.center_delay_ms) + std::max(0.0f, config_.depth_ms);
  const float max_delay_seconds = std::max(kMinDelayBufferSeconds, max_delay_ms * 0.001f);
  const int max_delay = static_cast<int>(sample_rate_ * static_cast<double>(max_delay_seconds)) + 1;
  for (auto& delay : delays_) {
    delay.prepare(max_delay);
  }
  lfos_[0].prepare(sample_rate_);
  lfos_[1].prepare(sample_rate_);
  lfos_[0].set_rate_hz(config_.rate_hz);
  lfos_[1].set_rate_hz(config_.rate_hz);
  reset();
}

void Chorus::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float delay_l = (config_.center_delay_ms + config_.depth_ms * lfos_[0].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    const float delay_r = (config_.center_delay_ms + config_.depth_ms * lfos_[1].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    left[i] = dry * in_l + wet * delays_[0].process(in_l, delay_l);
    right[i] = dry * in_r + wet * delays_[1].process(in_r, delay_r);
  }
}

bool Chorus::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      // Updates the LFO increment in place; preserves oscillator phase.
      lfos_[0].set_rate_hz(config_.rate_hz);
      lfos_[1].set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      config_.depth_ms = std::max(0.0f, value);
      return true;
    case 2:
      config_.center_delay_ms = std::max(0.0f, value);
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

void Chorus::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  lfos_[0].reset(0.0);
  lfos_[1].reset(0.25);
}

}  // namespace sonare::effects::modulation
