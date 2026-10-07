#include "effects/modulation/flanger.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "rt/scoped_no_denormals.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

using common::kMaxFeedback;
using common::kMaxModulationPreDelayMs;

namespace {

constexpr float kMaxFlangerDepthMs = 100.0f;
// Minimum delay-buffer length so the buffer is never smaller than a typical
// flanger range even for tiny configured delays.
constexpr float kMinDelayBufferMs = 100.0f;  // 100 ms
constexpr float kMaxPhaseDeg = 180.0f;
constexpr double kDegreesPerTurn = 360.0;
constexpr unsigned int kPreFilterModeCount = 3;

}  // namespace

Flanger::Flanger(FlangerConfig config) : config_(config) {
  config_.center_delay_ms = std::clamp(config_.center_delay_ms, 0.0f, kMaxModulationPreDelayMs);
  config_.depth_ms = std::clamp(config_.depth_ms, 0.0f, kMaxFlangerDepthMs);
  config_.phase_deg = std::clamp(config_.phase_deg, 0.0f, kMaxPhaseDeg);
}

void Flanger::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Size the buffer for the maximum AUTOMATABLE modulated delay, not just the
  // initial config: set_parameter clamps center and depth, so the LFO peak can
  // reach the sum of both ceilings. Sizing to the initial config would let later
  // automation exceed the buffer and be silently truncated by the delay-line
  // read clamp. The floor keeps a sane minimum. (The read clamp still prevents
  // any out-of-bounds access.)
  const float buffer_ms =
      std::max(kMinDelayBufferMs, kMaxModulationPreDelayMs + kMaxFlangerDepthMs);
  const int max_delay = static_cast<int>(sample_rate_ * static_cast<double>(buffer_ms) * 0.001) + 1;
  for (auto& delay : delays_) {
    delay.prepare(max_delay);
    delay.set_interpolation(config_.interpolation);
  }
  lfos_[0].prepare(sample_rate_);
  lfos_[1].prepare(sample_rate_);
  lfos_[0].set_rate_hz(config_.rate_hz);
  lfos_[1].set_rate_hz(config_.rate_hz);
  for (auto& pre_filter : pre_filters_) {
    pre_filter.prepare(config_.pre_filter_mode, config_.pre_filter_hz, sample_rate_);
  }
  reset();
}

void Flanger::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // Block-rate dry/wet + modulation depth: smoothed across blocks by the engine
  // parameter slot smoother, not per-sample (see Chorus::process for the rationale).
  const common::MixGains mix =
      common::mix_gains(config_.mix_law, std::clamp(config_.dry_wet, 0.0f, 1.0f));
  const float wet = mix.wet;
  const float dry = mix.dry;
  const float fb = std::clamp(config_.feedback, -kMaxFeedback, kMaxFeedback);
  // Derived from the rate in hertz each block, so the hold survives a rate change.
  const double step_per_sample =
      static_cast<double>(std::max(0.0f, config_.step_rate_hz)) / sample_rate_;
  if (step_per_sample <= 0.0) step_phase_ = 1.0;
  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    float lfo_l = lfos_[0].process();
    float lfo_r = lfos_[1].process();
    if (step_per_sample > 0.0) {
      if (step_phase_ >= 1.0) {
        step_phase_ -= std::floor(step_phase_);
        held_ = {lfo_l, lfo_r};
      }
      step_phase_ += step_per_sample;
      lfo_l = held_[0];
      lfo_r = held_[1];
    }
    const float delay_l = (config_.center_delay_ms + config_.depth_ms * lfo_l) * 0.001f *
                          static_cast<float>(sample_rate_);
    const float delay_r = (config_.center_delay_ms + config_.depth_ms * lfo_r) * 0.001f *
                          static_cast<float>(sample_rate_);
    // The section sits in front of the delay; the loop closes around the delay
    // alone, so the return re-enters after the filter rather than through it.
    const float wet_l =
        delays_[0].process(pre_filters_[0].process(in_l) + fb * feedback_[0], delay_l);
    const float wet_r =
        delays_[1].process(pre_filters_[1].process(in_r) + fb * feedback_[1], delay_r);
    feedback_ = {wet_l, wet_r};
    feedback_non_finite_ |= !std::isfinite(wet_l) || !std::isfinite(wet_r);
    if (stereo) {
      left[i] = dry * in_l + wet * wet_l;
      right[i] = dry * in_r + wet * wet_r;
    } else {
      // Mono: collapse the two LFO-modulated voices into the single output
      // buffer so it is not written twice with different values.
      left[i] = dry * in_l + wet * 0.5f * (wet_l + wet_r);
    }
  }
  discard_non_finite();
}

void Flanger::discard_non_finite() noexcept {
  if (!feedback_non_finite_) return;
  feedback_non_finite_ = false;
  static_cast<void>(discard_run_if_non_finite(feedback_.begin(), feedback_.end(), 0.0f));
  // Each line is fed by the feedback cell that reads it, so the poison
  // recirculates instead of flowing out. O(line), recovery only.
  for (auto& delay : delays_) delay.reset();
  // The section in front of the line holds a sample of its own, and a poisoned
  // one would re-contaminate the line the moment the next block arrived.
  for (auto& pre_filter : pre_filters_) pre_filter.reset();
  // Counted on the flag, not on the cell: the cell holds the block's last sample
  // and is often finite again while the line the reset above wiped still carried
  // the poison.
  note_non_finite_discard();
}

bool Flanger::set_parameter_impl(unsigned int param_id, float value) {
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
      config_.depth_ms = std::clamp(value, 0.0f, kMaxFlangerDepthMs);
      return true;
    case 2:
      config_.center_delay_ms = std::clamp(value, 0.0f, kMaxModulationPreDelayMs);
      return true;
    case 3:
      // process() clamps feedback to +-kMaxFeedback; store the raw target.
      config_.feedback = value;
      return true;
    case 4:
      config_.dry_wet = value;
      return true;
    case 5: {
      config_.pre_filter_hz = value;
      // Re-derives the pole from the new corner and keeps the running sample, so
      // a moved corner does not put a step into the loop.
      for (auto& pre_filter : pre_filters_) {
        const float carried = pre_filter.state();
        pre_filter.prepare(config_.pre_filter_mode, config_.pre_filter_hz, sample_rate_);
        pre_filter.set_state(carried);
      }
      return true;
    }
    case 6:
      config_.phase_deg = std::clamp(value, 0.0f, kMaxPhaseDeg);
      lfos_[1].reset(lfos_[0].phase() + static_cast<double>(config_.phase_deg) / kDegreesPerTurn);
      return true;
    case 7:
      config_.step_rate_hz = std::max(0.0f, value);
      return true;
    case 8: {
      // An unnamed mode is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kPreFilterModeCount)) {
        return false;
      }
      config_.pre_filter_mode = static_cast<PreFilterMode>(static_cast<int>(value));
      for (auto& pre_filter : pre_filters_) {
        const float carried = pre_filter.state();
        pre_filter.prepare(config_.pre_filter_mode, config_.pre_filter_hz, sample_rate_);
        pre_filter.set_state(carried);
      }
      return true;
    }
    case 9: {
      if (!delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<DelayInterpolation>(static_cast<int>(value));
      for (auto& delay : delays_) delay.set_interpolation(config_.interpolation);
      return true;
    }
    case 10:
      return common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

bool Flanger::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar/coefficient update; the
  // delay lines are pre-sized to the clamped range at prepare(), so no id
  // allocates or resets audio state. Unknown ids are rejected by set_parameter.
  return param_id <= 10;
}

std::vector<rt::ParamDescriptor> Flanger::parameter_descriptors() const {
  return {{"rateHz", 0},        {"depthMs", 1},       {"centerDelayMs", 2}, {"feedback", 3},
          {"dryWet", 4},        {"preFilterHz", 5},   {"phaseDeg", 6},      {"stepRateHz", 7},
          {"preFilterMode", 8}, {"interpolation", 9}, {"mixLaw", 10}};
}

void Flanger::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  for (auto& pre_filter : pre_filters_) {
    pre_filter.reset();
  }
  lfos_[0].reset(0.0);
  lfos_[1].reset(static_cast<double>(config_.phase_deg) / kDegreesPerTurn);
  step_phase_ = 1.0;
  held_ = {0.0f, 0.0f};
  feedback_ = {0.0f, 0.0f};
  feedback_non_finite_ = false;
}

}  // namespace sonare::effects::modulation
