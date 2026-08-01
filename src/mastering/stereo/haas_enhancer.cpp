#include "mastering/stereo/haas_enhancer.h"

#include <algorithm>
#include <cmath>

#include "core/audio.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::stereo {

HaasEnhancer::HaasEnhancer(HaasEnhancerConfig config) : config_(config) {
  validate_config(config_);
}

void HaasEnhancer::prepare(double sample_rate, int max_block_size) {
  if (!std::isfinite(sample_rate) || sample_rate < kMinAudioSampleRate ||
      sample_rate > kMaxAudioSampleRate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate is outside supported bounds");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  rebuild_delay();
}

void HaasEnhancer::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "HaasEnhancer");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }
  if (num_channels < 2 || delay_samples_ == 0 || config_.mix == 0.0f) {
    return;
  }

  const int delayed_ch = config_.delay_right ? 1 : 0;
  const int source_ch = config_.delay_right ? 0 : 1;
  for (int i = 0; i < num_samples; ++i) {
    const float delayed = process_delay(channels[source_ch][i]);
    channels[delayed_ch][i] =
        channels[delayed_ch][i] * (1.0f - config_.mix) + delayed * config_.mix;
  }
}

void HaasEnhancer::reset() {
  std::fill(delay_.begin(), delay_.end(), 0.0f);
  delay_index_ = 0;
}

int HaasEnhancer::tail_samples() const noexcept {
  // The delayed cross-channel path is finite: after input becomes silent, the
  // last sample emerges exactly delay_samples_ later. A dry-only or zero-delay
  // configuration never contributes delayed output.
  return prepared_ && config_.mix > 0.0f ? std::max(0, delay_samples_) : 0;
}

void HaasEnhancer::set_config(const HaasEnhancerConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    rebuild_delay();
  }
}

bool HaasEnhancer::set_parameter(unsigned int param_id, float value) {
  if (!std::isfinite(value)) return false;
  switch (param_id) {
    case 0:
      if (value > 1000.0f) return false;
      config_.delay_ms = std::max(0.0f, value);
      // Changing the delay length requires resizing the delay line, which
      // reallocates and clears the buffered samples.
      if (prepared_) {
        rebuild_delay();
      }
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> HaasEnhancer::parameter_descriptors() const {
  return {{"delayMs", 0}, {"mix", 1}};
}

bool HaasEnhancer::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // delay_ms reallocates the delay line; everything else is an in-place scalar.
  return param_id != 0;
}

void HaasEnhancer::validate_config(const HaasEnhancerConfig& config) {
  constexpr float kMaxDelayMs = 1000.0f;
  if (!std::isfinite(config.delay_ms) || config.delay_ms < 0.0f || config.delay_ms > kMaxDelayMs ||
      !std::isfinite(config.mix) || config.mix < 0.0f || config.mix > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid Haas enhancer configuration");
  }
}

void HaasEnhancer::rebuild_delay() {
  delay_samples_ = static_cast<int>(std::round(sample_rate_ * config_.delay_ms * 0.001));
  delay_.assign(static_cast<size_t>(std::max(delay_samples_, 1)), 0.0f);
  delay_index_ = 0;
}

float HaasEnhancer::process_delay(float input) {
  float delayed = delay_[delay_index_];
  delay_[delay_index_] = input;
  delay_index_ = (delay_index_ + 1) % delay_.size();
  return delayed;
}

}  // namespace sonare::mastering::stereo
