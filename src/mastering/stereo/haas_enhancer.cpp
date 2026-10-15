#include "mastering/stereo/haas_enhancer.h"

#include <algorithm>
#include <cmath>

#include "core/audio.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
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
  // Out of the path for a mono block, no delay or a zero mix; back in from rest.
  if (!stage_.admit(num_channels >= 2 && delay_samples_ != 0 && config_.mix != 0.0f,
                    [this] { reset(); })) {
    return;
  }

  const int delayed_ch = config_.delay_right ? 1 : 0;
  const int source_ch = config_.delay_right ? 0 : 1;
  delay_.route(config_.delay_right);
  for (int i = 0; i < num_samples; ++i) {
    const float delayed = delay_.exchange(channels[source_ch][i]);
    channels[delayed_ch][i] =
        channels[delayed_ch][i] * (1.0f - config_.mix) + delayed * config_.mix;
  }
}

void HaasEnhancer::reset() { delay_.clear(); }

int HaasEnhancer::tail_samples() const noexcept {
  // The delayed cross-channel path is finite: after input becomes silent, the
  // last sample emerges exactly delay_samples_ later. A dry-only or zero-delay
  // configuration never contributes delayed output.
  rt::TailBudget tail;
  if (prepared_ && config_.mix > 0.0f) tail.delay(delay_samples_);
  return tail.samples();
}

void HaasEnhancer::set_config(const HaasEnhancerConfig& config) {
  validate_config(config);
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<HaasEnhancerConfig>&) {
    // The line is rebuilt, and its audio dropped, only for a new length.
    if (prepared_ &&
        static_cast<int>(std::round(sample_rate_ * config_.delay_ms * 0.001)) != delay_samples_) {
      rebuild_delay();
    }
  });
}

namespace {
constexpr float kMaxDelayMs = 1000.0f;

bool delay_valid(float v) noexcept { return std::isfinite(v) && v >= 0.0f && v <= kMaxDelayMs; }
bool mix_valid(float v) noexcept { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }
}  // namespace

bool HaasEnhancer::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!delay_valid(value)) return false;
      config_.delay_ms = value;
      // Changing the delay length requires resizing the delay line, which
      // reallocates and clears the buffered samples.
      if (prepared_) {
        rebuild_delay();
      }
      return true;
    case 1:
      if (!mix_valid(value)) return false;
      config_.mix = value;
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
  if (!delay_valid(config.delay_ms) || !mix_valid(config.mix)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid Haas enhancer configuration");
  }
}

void HaasEnhancer::rebuild_delay() {
  delay_samples_ = static_cast<int>(std::round(sample_rate_ * config_.delay_ms * 0.001));
  delay_.assign(static_cast<size_t>(std::max(delay_samples_, 1)));
}

}  // namespace sonare::mastering::stereo
