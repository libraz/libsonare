#include "effects/delay/stereo_delay.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"

namespace sonare::effects::delay {
namespace {

constexpr float kDelaySmoothingTimeSeconds = 0.010f;

float config_delay_samples(float delay_ms, double sample_rate) noexcept {
  return std::max(0.0f, delay_ms) * 0.001f * static_cast<float>(sample_rate);
}

}  // namespace

StereoDelay::StereoDelay(StereoDelayConfig config) : config_(config) {}

void StereoDelay::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const int max_delay = static_cast<int>(sample_rate_ * 4.0);
  for (auto& delay : delays_) {
    delay.prepare(max_delay);
  }
  reset();
}

void StereoDelay::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float feedback = std::clamp(config_.feedback, 0.0f, 0.95f);
  const float ping_pong = std::clamp(config_.ping_pong, 0.0f, 1.0f);
  const std::array<float, 2> target_delay_samples{
      config_delay_samples(config_.delay_time_l_ms, sample_rate_),
      config_delay_samples(config_.delay_time_r_ms, sample_rate_)};
  const float smoothing_coeff = std::clamp(
      1.0f / std::max(1.0f, static_cast<float>(sample_rate_) * kDelaySmoothingTimeSeconds), 0.0f,
      1.0f);

  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  for (int i = 0; i < num_samples; ++i) {
    delay_samples_[0] += (target_delay_samples[0] - delay_samples_[0]) * smoothing_coeff;
    delay_samples_[1] += (target_delay_samples[1] - delay_samples_[1]) * smoothing_coeff;
    const float in_l = left[i];
    const float in_r = right[i];
    const float feed_l = in_l + feedback * ((1.0f - ping_pong) * feedback_state_[0] +
                                            ping_pong * feedback_state_[1]);
    const float feed_r = in_r + feedback * ((1.0f - ping_pong) * feedback_state_[1] +
                                            ping_pong * feedback_state_[0]);
    const float delayed_l = delays_[0].process(feed_l, delay_samples_[0]);
    const float delayed_r = delays_[1].process(feed_r, delay_samples_[1]);
    feedback_state_ = {delayed_l, delayed_r};
    if (stereo) {
      left[i] = dry * in_l + wet * delayed_l;
      right[i] = dry * in_r + wet * delayed_r;
    } else {
      // Mono: collapse the two delay taps into the single output buffer so it
      // is not written twice with different values.
      left[i] = dry * in_l + wet * 0.5f * (delayed_l + delayed_r);
    }
  }
}

void StereoDelay::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  delay_samples_ = {config_delay_samples(config_.delay_time_l_ms, sample_rate_),
                    config_delay_samples(config_.delay_time_r_ms, sample_rate_)};
  feedback_state_ = {0.0f, 0.0f};
}

void StereoDelay::set_config(const StereoDelayConfig& config) noexcept { config_ = config; }

bool StereoDelay::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // process() smooths delay-time automation before it reaches the
      // fractional read taps; the lines are sized for up to 4 s.
      config_.delay_time_l_ms = std::max(0.0f, value);
      return true;
    case 1:
      config_.delay_time_r_ms = std::max(0.0f, value);
      return true;
    case 2:
      // process() clamps feedback to [0, 0.95]; store the raw target.
      config_.feedback = value;
      return true;
    case 3:
      // process() clamps ping_pong to [0, 1]; store the raw target.
      config_.ping_pong = value;
      return true;
    case 4:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

}  // namespace sonare::effects::delay
