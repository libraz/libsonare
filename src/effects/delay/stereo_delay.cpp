#include "effects/delay/stereo_delay.h"

#include <algorithm>

namespace sonare::effects::delay {

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
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float feedback = std::clamp(config_.feedback, 0.0f, 0.95f);
  const float ping_pong = std::clamp(config_.ping_pong, 0.0f, 1.0f);
  const float delay_l = config_.delay_time_l_ms * 0.001f * static_cast<float>(sample_rate_);
  const float delay_r = config_.delay_time_r_ms * 0.001f * static_cast<float>(sample_rate_);

  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float feed_l = in_l + feedback * ((1.0f - ping_pong) * feedback_state_[0] +
                                            ping_pong * feedback_state_[1]);
    const float feed_r = in_r + feedback * ((1.0f - ping_pong) * feedback_state_[1] +
                                            ping_pong * feedback_state_[0]);
    const float delayed_l = delays_[0].process(feed_l, delay_l);
    const float delayed_r = delays_[1].process(feed_r, delay_r);
    feedback_state_ = {delayed_l, delayed_r};
    left[i] = dry * in_l + wet * delayed_l;
    right[i] = dry * in_r + wet * delayed_r;
  }
}

void StereoDelay::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  feedback_state_ = {0.0f, 0.0f};
}

void StereoDelay::set_config(const StereoDelayConfig& config) noexcept { config_ = config; }

}  // namespace sonare::effects::delay
