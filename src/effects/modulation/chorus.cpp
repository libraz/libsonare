#include "effects/modulation/chorus.h"

#include <algorithm>

#include "rt/scoped_no_denormals.h"

namespace sonare::effects::modulation {

namespace {
// Minimum delay-buffer length so the buffer is never smaller than a typical
// chorus range even for tiny configured delays.
constexpr float kMinDelayBufferSeconds = 0.1f;  // 100 ms
// Maximum automatable center delay / modulation depth (each). set_parameter
// clamps to this so the LFO peak (center + depth) stays within the buffer the
// prepare() pass sizes for, and a later automation cannot be silently truncated.
constexpr float kMaxChorusDelayMs = 50.0f;
}  // namespace

Chorus::Chorus(ChorusConfig config) : config_(config) {
  // Apply the same delay clamp the automation path (set_parameter) enforces so
  // the construction path can never request a center/depth larger than the
  // buffer prepare() sizes for. Without this, an out-of-range constructed delay
  // would be silently truncated by the ModDelayLine read clamp instead of
  // clamped consistently with set_parameter.
  config_.center_delay_ms = std::clamp(config_.center_delay_ms, 0.0f, kMaxChorusDelayMs);
  config_.depth_ms = std::clamp(config_.depth_ms, 0.0f, kMaxChorusDelayMs);
}

void Chorus::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Size the buffer for the maximum AUTOMATABLE modulated delay (center + depth,
  // each clamped to kMaxChorusDelayMs by set_parameter), not just the initial
  // config, so later automation up to the clamped range is fully representable
  // rather than silently truncated by the delay-line read clamp. The 100 ms
  // floor keeps a sane minimum. (The read clamp still prevents OOB access.)
  const float max_delay_ms = 2.0f * kMaxChorusDelayMs;
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
  rt::ScopedNoDenormals no_denormals;
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // dry/wet and modulation depth are read once per block (not per-sample
  // smoothed); zipper-free automation relies on the engine's parameter slot
  // smoother ramping config_ across blocks. A direct RT command bypasses that
  // smoother, so very fast large jumps on a big block may zipper faintly.
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float delay_l = (config_.center_delay_ms + config_.depth_ms * lfos_[0].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    const float delay_r = (config_.center_delay_ms + config_.depth_ms * lfos_[1].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    const float wet_l = delays_[0].process(in_l, delay_l);
    const float wet_r = delays_[1].process(in_r, delay_r);
    if (stereo) {
      left[i] = dry * in_l + wet * wet_l;
      right[i] = dry * in_r + wet * wet_r;
    } else {
      // Mono: collapse the two LFO-modulated voices into the single output
      // buffer so it is not written twice with different values.
      left[i] = dry * in_l + wet * 0.5f * (wet_l + wet_r);
    }
  }
}

bool Chorus::set_parameter(unsigned int param_id, float value) {
  // Reject before the clamps below: std::clamp leaves NaN intact and the delay
  // ids feed the fractional read index (see delay_param_acceptable).
  if (!delay_param_acceptable(value)) return false;
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      // Updates the LFO increment in place; preserves oscillator phase.
      lfos_[0].set_rate_hz(config_.rate_hz);
      lfos_[1].set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      config_.depth_ms = std::clamp(value, 0.0f, kMaxChorusDelayMs);
      return true;
    case 2:
      config_.center_delay_ms = std::clamp(value, 0.0f, kMaxChorusDelayMs);
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

bool Chorus::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar/coefficient update; the
  // delay lines are pre-sized to kMaxChorusDelayMs at prepare(), so no id
  // allocates or resets audio state. Unknown ids are rejected by set_parameter.
  return param_id <= 3;
}

std::vector<rt::ParamDescriptor> Chorus::parameter_descriptors() const {
  return {{"rateHz", 0}, {"depthMs", 1}, {"centerDelayMs", 2}, {"dryWet", 3}};
}

void Chorus::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  lfos_[0].reset(0.0);
  lfos_[1].reset(0.25);
}

}  // namespace sonare::effects::modulation
