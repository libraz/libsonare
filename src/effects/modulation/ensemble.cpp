#include "effects/modulation/ensemble.h"

#include <algorithm>
#include <cmath>

namespace sonare::effects::modulation {

namespace {
constexpr float kMaxDepthMs = 10.0f;
constexpr float kMaxCenterDelayMs = 25.0f;
constexpr float kMinDelayBufferSeconds = 0.1f;  // 100 ms, matching Chorus
}  // namespace

Ensemble::Ensemble(EnsembleConfig config) : config_(config) {
  config_.depth_slow_ms = std::clamp(config_.depth_slow_ms, 0.0f, kMaxDepthMs);
  config_.depth_fast_ms = std::clamp(config_.depth_fast_ms, 0.0f, kMaxDepthMs);
  config_.center_delay_ms = std::clamp(config_.center_delay_ms, 0.0f, kMaxCenterDelayMs);
  config_.tone_hz = std::clamp(config_.tone_hz, 500.0f, 20000.0f);
  config_.dry_wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
}

void Ensemble::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Size for the maximum automatable modulated delay so later automation is
  // never silently truncated by the delay-line read clamp.
  const float max_delay_ms = kMaxCenterDelayMs + 2.0f * kMaxDepthMs;
  const float max_delay_seconds = std::max(kMinDelayBufferSeconds, max_delay_ms * 0.001f);
  const int max_delay = static_cast<int>(sample_rate_ * static_cast<double>(max_delay_seconds)) + 1;
  for (auto& delay : delays_) delay.prepare(max_delay);
  for (size_t tap = 0; tap < 3; ++tap) {
    slow_lfos_[tap].prepare(sample_rate_);
    fast_lfos_[tap].prepare(sample_rate_);
    slow_lfos_[tap].set_rate_hz(config_.rate_slow_hz);
    fast_lfos_[tap].set_rate_hz(config_.rate_fast_hz);
  }
  reset();
}

void Ensemble::reset() {
  for (auto& delay : delays_) delay.reset();
  // The 3-phase pattern: taps 120 degrees apart on BOTH LFOs.
  for (size_t tap = 0; tap < 3; ++tap) {
    slow_lfos_[tap].reset(static_cast<double>(tap) / 3.0);
    fast_lfos_[tap].reset(static_cast<double>(tap) / 3.0);
  }
  tone_state_.fill(0.0f);
}

void Ensemble::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float ms_to_samples = 0.001f * static_cast<float>(sample_rate_);
  const float tone_alpha = std::clamp(
      1.0f - std::exp(-6.28318530718f * config_.tone_hz / static_cast<float>(sample_rate_)), 0.01f,
      1.0f);

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    float slow[3];
    float fast[3];
    for (size_t tap = 0; tap < 3; ++tap) {
      slow[tap] = slow_lfos_[tap].process();
      fast[tap] = fast_lfos_[tap].process();
    }
    float wet_l = 0.0f;
    float wet_r = 0.0f;
    for (size_t tap = 0; tap < 3; ++tap) {
      const float sweep = config_.depth_slow_ms * slow[tap] + config_.depth_fast_ms * fast[tap];
      // Right channel: same 3-phase pattern with inverted LFO polarity.
      const float delay_l = (config_.center_delay_ms + sweep) * ms_to_samples;
      const float delay_r = (config_.center_delay_ms - sweep) * ms_to_samples;
      wet_l += delays_[tap].process(in_l, std::max(0.0f, delay_l));
      wet_r += delays_[3 + tap].process(in_r, std::max(0.0f, delay_r));
    }
    wet_l *= 1.0f / 3.0f;
    wet_r *= 1.0f / 3.0f;
    // BBD bandwidth: gentle one-pole lowpass on the wet path only.
    tone_state_[0] += tone_alpha * (wet_l - tone_state_[0]);
    tone_state_[1] += tone_alpha * (wet_r - tone_state_[1]);
    if (stereo) {
      left[i] = dry * in_l + wet * tone_state_[0];
      right[i] = dry * in_r + wet * tone_state_[1];
    } else {
      left[i] = dry * in_l + wet * 0.5f * (tone_state_[0] + tone_state_[1]);
    }
  }
}

bool Ensemble::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_slow_hz = std::max(0.0f, value);
      for (auto& lfo : slow_lfos_) lfo.set_rate_hz(config_.rate_slow_hz);
      return true;
    case 1:
      config_.rate_fast_hz = std::max(0.0f, value);
      for (auto& lfo : fast_lfos_) lfo.set_rate_hz(config_.rate_fast_hz);
      return true;
    case 2:
      config_.depth_slow_ms = std::clamp(value, 0.0f, kMaxDepthMs);
      return true;
    case 3:
      config_.depth_fast_ms = std::clamp(value, 0.0f, kMaxDepthMs);
      return true;
    case 4:
      config_.center_delay_ms = std::clamp(value, 0.0f, kMaxCenterDelayMs);
      return true;
    case 5:
      config_.tone_hz = std::clamp(value, 500.0f, 20000.0f);
      return true;
    case 6:
      config_.dry_wet = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Ensemble::parameter_descriptors() const {
  return {{"rateSlowHz", 0},    {"rateFastHz", 1}, {"depthSlowMs", 2}, {"depthFastMs", 3},
          {"centerDelayMs", 4}, {"toneHz", 5},     {"dryWet", 6}};
}

}  // namespace sonare::effects::modulation
