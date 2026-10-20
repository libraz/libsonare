#include "effects/modulation/ensemble.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "effects/common/field_domain.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

using common::FieldRange;
using common::in_domain;
using common::kNoUpperBound;
using common::require_in_domain;

using constants::kDefaultDawSampleRate;

using common::kMaxModulationPreDelayMs;
using constants::kTwoPiD;

namespace {
constexpr float kMaxDepthMs = 10.0f;
constexpr float kMaxPreDelayDevMs = 20.0f;
constexpr float kMaxDepthDev = 1.0f;
constexpr float kMinDelayBufferSeconds = 0.1f;  // 100 ms, matching Chorus

// Field domains shared by validate_config and live automation.
constexpr FieldRange kRateHz{0.0f, kNoUpperBound};
constexpr FieldRange kDepthMs{0.0f, kMaxDepthMs};
constexpr FieldRange kCenterDelayMs{0.0f, kMaxModulationPreDelayMs};
constexpr FieldRange kToneHz{500.0f, 20000.0f};
constexpr FieldRange kDryWet{0.0f, 1.0f};
constexpr FieldRange kPreDelayDevMs{0.0f, kMaxPreDelayDevMs};
constexpr FieldRange kDepthDev{-kMaxDepthDev, kMaxDepthDev};
constexpr FieldRange kPanDev{0.0f, 1.0f};
}  // namespace

Ensemble::Ensemble(EnsembleConfig config) : config_(config) { validate_config(config_); }

void Ensemble::validate_config(const EnsembleConfig& config) {
  require_in_domain("ensemble", "rate_slow_hz", config.rate_slow_hz, kRateHz);
  require_in_domain("ensemble", "rate_fast_hz", config.rate_fast_hz, kRateHz);
  require_in_domain("ensemble", "depth_slow_ms", config.depth_slow_ms, kDepthMs);
  require_in_domain("ensemble", "depth_fast_ms", config.depth_fast_ms, kDepthMs);
  require_in_domain("ensemble", "center_delay_ms", config.center_delay_ms, kCenterDelayMs);
  require_in_domain("ensemble", "tone_hz", config.tone_hz, kToneHz);
  require_in_domain("ensemble", "dry_wet", config.dry_wet, kDryWet);
  require_in_domain("ensemble", "rate_hz", config.rate_hz, kRateHz);
  require_in_domain("ensemble", "pre_delay_dev_ms", config.pre_delay_dev_ms, kPreDelayDevMs);
  require_in_domain("ensemble", "depth_dev", config.depth_dev, kDepthDev);
  require_in_domain("ensemble", "pan_dev", config.pan_dev, kPanDev);
}

void Ensemble::apply_rates() noexcept {
  float slow = config_.rate_slow_hz;
  float fast = config_.rate_fast_hz;
  if (config_.rate_hz > 0.0f) {
    const EnsembleConfig defaults;
    const float ratio = config_.rate_slow_hz > 0.0f ? config_.rate_fast_hz / config_.rate_slow_hz
                                                    : defaults.rate_fast_hz / defaults.rate_slow_hz;
    slow = config_.rate_hz;
    fast = config_.rate_hz * ratio;
  }
  for (auto& lfo : slow_lfos_) lfo.set_rate_hz(slow);
  for (auto& lfo : fast_lfos_) lfo.set_rate_hz(fast);
}

void Ensemble::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  // Size for the maximum automatable modulated delay so later automation is
  // never silently truncated by the delay-line read clamp: the outer voice sits
  // the deviation past the centre, and its sweep is both depths scaled by the
  // depth deviation.
  const float max_delay_ms =
      kMaxModulationPreDelayMs + kMaxPreDelayDevMs + 2.0f * kMaxDepthMs * (1.0f + kMaxDepthDev);
  const float max_delay_seconds = std::max(kMinDelayBufferSeconds, max_delay_ms * 0.001f);
  const int max_delay = static_cast<int>(sample_rate_ * static_cast<double>(max_delay_seconds)) + 1;
  for (auto& delay : delays_) {
    delay.prepare(max_delay);
    delay.set_interpolation(config_.interpolation);
  }
  for (size_t tap = 0; tap < 3; ++tap) {
    slow_lfos_[tap].prepare(sample_rate_);
    fast_lfos_[tap].prepare(sample_rate_);
  }
  apply_rates();
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
  rt::ScopedNoDenormals no_denormals;
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // Block-rate dry/wet + modulation depth: smoothed across blocks by the engine
  // parameter slot smoother, not per-sample (see Chorus::process for the rationale).
  const common::MixGains mix = common::mix_gains(config_.mix_law, config_.dry_wet);
  const float wet = mix.wet;
  const float dry = mix.dry;
  const float ms_to_samples = 0.001f * static_cast<float>(sample_rate_);
  const float tone_alpha = rt::one_pole_lowpass_alpha_matched(config_.tone_hz, sample_rate_);

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
      const float spread = static_cast<float>(tap) - 1.0f;  // -1, 0, +1 across the voices
      const float pre_ms = config_.pre_delay_dev_ms * spread;
      const float depth_scale = 1.0f + config_.depth_dev * spread;
      const float pan = config_.pan_dev * spread;
      const float gain_l = std::min(1.0f, 1.0f - pan);
      const float gain_r = std::min(1.0f, 1.0f + pan);
      const float sweep =
          (config_.depth_slow_ms * slow[tap] + config_.depth_fast_ms * fast[tap]) * depth_scale;
      // Right channel: same 3-phase pattern with inverted LFO polarity.
      const float delay_l = (config_.center_delay_ms + pre_ms + sweep) * ms_to_samples;
      const float delay_r = (config_.center_delay_ms + pre_ms - sweep) * ms_to_samples;
      wet_l += gain_l * delays_[tap].process(in_l, std::max(0.0f, delay_l));
      wet_r += gain_r * delays_[3 + tap].process(in_r, std::max(0.0f, delay_r));
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
  discard_non_finite();
}

int Ensemble::tail_samples() const noexcept {
  const common::MixGains mix = common::mix_gains(config_.mix_law, config_.dry_wet);
  if (!(mix.wet > 0.0f)) return 0;
  // The outer voice's longest read, then the tone low-pass after the taps.
  const double sweep_ms = (config_.depth_slow_ms + config_.depth_fast_ms) *
                          (1.0 + std::fabs(static_cast<double>(config_.depth_dev)));
  const double longest_ms = config_.center_delay_ms + config_.pre_delay_dev_ms + sweep_ms;
  const double tone_alpha =
      std::min(1.0, 1.0 - std::exp(-kTwoPiD * config_.tone_hz / sample_rate_));
  rt::TailBudget tail;
  tail.delay(longest_ms * 0.001 * sample_rate_ + kDelayReadStencilSamples).decay(1.0 - tone_alpha);
  return tail.samples();
}

void Ensemble::discard_non_finite() noexcept {
  // The tap delays are fed by the input alone, so a non-finite sample leaves
  // them within one line length; the tone filter is where it would stay.
  if (discard_run_if_non_finite(tone_state_.begin(), tone_state_.end(), 0.0f)) {
    note_non_finite_discard();
  }
}

bool Ensemble::set_parameter_impl(unsigned int param_id, float value) {
  // A value outside a field's domain is refused with no state change; non-finite
  // values never reach the delay read index (see delay_param_acceptable).
  if (!delay_param_acceptable(value)) return false;
  switch (param_id) {
    case 0:
      if (!in_domain(value, kRateHz)) return false;
      config_.rate_slow_hz = value;
      apply_rates();
      return true;
    case 1:
      if (!in_domain(value, kRateHz)) return false;
      config_.rate_fast_hz = value;
      apply_rates();
      return true;
    case 2:
      if (!in_domain(value, kDepthMs)) return false;
      config_.depth_slow_ms = value;
      return true;
    case 3:
      if (!in_domain(value, kDepthMs)) return false;
      config_.depth_fast_ms = value;
      return true;
    case 4:
      if (!in_domain(value, kCenterDelayMs)) return false;
      config_.center_delay_ms = value;
      return true;
    case 5:
      if (!in_domain(value, kToneHz)) return false;
      config_.tone_hz = value;
      return true;
    case 6:
      if (!in_domain(value, kDryWet)) return false;
      config_.dry_wet = value;
      return true;
    case 7:
      if (!in_domain(value, kRateHz)) return false;
      config_.rate_hz = value;
      apply_rates();
      return true;
    case 8:
      if (!in_domain(value, kPreDelayDevMs)) return false;
      config_.pre_delay_dev_ms = value;
      return true;
    case 9:
      if (!in_domain(value, kDepthDev)) return false;
      config_.depth_dev = value;
      return true;
    case 10:
      if (!in_domain(value, kPanDev)) return false;
      config_.pan_dev = value;
      return true;
    case 11: {
      if (!delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<DelayInterpolation>(static_cast<int>(value));
      for (auto& delay : delays_) delay.set_interpolation(config_.interpolation);
      return true;
    }
    case 12:
      return common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

bool Ensemble::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar/coefficient update; the
  // delay lines are pre-sized to the accepted range at prepare(), so no id
  // allocates or resets audio state. Unknown ids are rejected by
  // set_parameter.
  return param_id <= 12;
}

std::vector<rt::ParamDescriptor> Ensemble::parameter_descriptors() const {
  return {{"rateSlowHz", 0},    {"rateFastHz", 1}, {"depthSlowMs", 2}, {"depthFastMs", 3},
          {"centerDelayMs", 4}, {"toneHz", 5},     {"dryWet", 6},      {"rateHz", 7},
          {"preDelayDevMs", 8}, {"depthDev", 9},   {"panDev", 10},     {"interpolation", 11},
          {"mixLaw", 12}};
}

}  // namespace sonare::effects::modulation
