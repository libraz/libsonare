#include "effects/modulation/chorus.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "effects/common/field_domain.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

using common::FieldRange;
using common::in_domain;
using common::kAnyFinite;
using common::kNoUpperBound;
using common::require_in_domain;

using constants::kDefaultDawSampleRate;
using constants::kDegreesPerTurnD;

using common::kMaxFeedback;
using common::kMaxModulationPreDelayMs;

namespace {
// Minimum delay-buffer length so the buffer is never smaller than a typical
// chorus range even for tiny configured delays.
constexpr float kMinDelayBufferSeconds = 0.1f;  // 100 ms
// Maximum automatable modulation depth. The centre delay takes the shared
// pre-delay ceiling; both bounds keep the LFO peak (center + depth) within the
// buffer prepare() sizes, so a later automation cannot be silently truncated.
constexpr float kMaxChorusDepthMs = 50.0f;
// Corner of the low-pass in the feedback return.
constexpr float kFeedbackCornerHz = 6000.0f;
constexpr unsigned int kPreFilterModeCount = 3;

// Field domains shared by validate_config and live automation.
constexpr FieldRange kRateHz{0.0f, kNoUpperBound};
constexpr FieldRange kDepthMs{0.0f, kMaxChorusDepthMs};
constexpr FieldRange kCenterDelayMs{0.0f, kMaxModulationPreDelayMs};
constexpr FieldRange kDryWet{0.0f, 1.0f};
constexpr FieldRange kFeedback{-kMaxFeedback, kMaxFeedback};
constexpr FieldRange kPhaseDeg{0.0f, 180.0f};
}  // namespace

Chorus::Chorus(ChorusConfig config) : config_(config) { validate_config(config_); }

void Chorus::validate_config(const ChorusConfig& config) {
  require_in_domain("chorus", "rate_hz", config.rate_hz, kRateHz);
  require_in_domain("chorus", "depth_ms", config.depth_ms, kDepthMs);
  require_in_domain("chorus", "center_delay_ms", config.center_delay_ms, kCenterDelayMs);
  require_in_domain("chorus", "dry_wet", config.dry_wet, kDryWet);
  require_in_domain("chorus", "pre_filter_hz", config.pre_filter_hz, kAnyFinite);
  require_in_domain("chorus", "feedback", config.feedback, kFeedback);
  require_in_domain("chorus", "phase_deg", config.phase_deg, kPhaseDeg);
}

void Chorus::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  // Size the buffer for the maximum AUTOMATABLE modulated delay (center + depth,
  // each bounded by its domain), not just the initial config, so later
  // automation up to the accepted range is fully representable rather than
  // silently truncated by the delay-line read clamp. The 100 ms floor keeps a
  // sane minimum. (The read clamp still prevents OOB access.)
  const float max_delay_ms = kMaxModulationPreDelayMs + kMaxChorusDepthMs;
  const float max_delay_seconds = std::max(kMinDelayBufferSeconds, max_delay_ms * 0.001f);
  const int max_delay = static_cast<int>(sample_rate_ * static_cast<double>(max_delay_seconds)) + 1;
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
  for (auto& filter : feedback_filters_) {
    filter.prepare(PreFilterMode::kLowPass, kFeedbackCornerHz, sample_rate_);
  }
  reset();
}

void Chorus::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  for (size_t ch = 0; ch < pre_filters_.size(); ++ch) {
    PreFilter& pre_filter = pre_filters_[ch];
    pre_filter_gates_[ch].admit(pre_filter.active(), [&pre_filter] { pre_filter.reset(); });
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // dry/wet and modulation depth are read once per block (not per-sample
  // smoothed); zipper-free automation relies on the engine's parameter slot
  // smoother ramping config_ across blocks. A direct RT command bypasses that
  // smoother, so very fast large jumps on a big block may zipper faintly.
  const common::MixGains mix = common::mix_gains(config_.mix_law, config_.dry_wet);
  const float wet = mix.wet;
  const float dry = mix.dry;
  const float fb = config_.feedback;
  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float delay_l = (config_.center_delay_ms + config_.depth_ms * lfos_[0].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    const float delay_r = (config_.center_delay_ms + config_.depth_ms * lfos_[1].process()) *
                          0.001f * static_cast<float>(sample_rate_);
    // The section sits in front of the delay, not across the dry path.
    float feed_l = pre_filters_[0].process(in_l);
    float feed_r = pre_filters_[1].process(in_r);
    // Skipped at zero gain so an untouched chorus stays bit-identical.
    if (fb != 0.0f) {
      feed_l += fb * feedback_[0];
      feed_r += fb * feedback_[1];
    }
    const float wet_l = delays_[0].process(feed_l, delay_l);
    const float wet_r = delays_[1].process(feed_r, delay_r);
    feedback_[0] = feedback_filters_[0].process(wet_l);
    feedback_[1] = feedback_filters_[1].process(wet_r);
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

int Chorus::tail_samples() const noexcept {
  const common::MixGains mix = common::mix_gains(config_.mix_law, config_.dry_wet);
  if (!(mix.wet > 0.0f)) return 0;
  // The longest modulated read, recirculating through the feedback low-pass.
  const double longest = (config_.center_delay_ms + config_.depth_ms) * 0.001 * sample_rate_ +
                         kDelayReadStencilSamples;
  rt::TailBudget feedback_ring;
  feedback_ring.decay(feedback_filters_[0].pole());
  rt::TailBudget tail;
  tail.decay(pre_filters_[0].pole())
      .delay(longest)
      .recirculation(longest + feedback_ring.samples(), config_.feedback);
  return tail.samples();
}

void Chorus::discard_non_finite() noexcept {
  // The section is recursive, so one non-finite sample would stay in it for
  // good; the line only carries what the section already let through.
  if (std::isfinite(pre_filters_[0].state()) && std::isfinite(pre_filters_[1].state()) &&
      std::isfinite(feedback_filters_[0].state()) && std::isfinite(feedback_filters_[1].state()) &&
      std::isfinite(feedback_[0]) && std::isfinite(feedback_[1])) {
    return;
  }
  for (auto& pre_filter : pre_filters_) pre_filter.reset();
  for (auto& gate : pre_filter_gates_) gate.close();
  for (auto& filter : feedback_filters_) filter.reset();
  feedback_.fill(0.0f);
  for (auto& delay : delays_) delay.reset();
  note_non_finite_discard();
}

bool Chorus::set_parameter_impl(unsigned int param_id, float value) {
  // A value outside a field's domain is refused with no state change; non-finite
  // values never reach the delay read index (see delay_param_acceptable).
  if (!delay_param_acceptable(value)) return false;
  switch (param_id) {
    case 0:
      if (!in_domain(value, kRateHz)) return false;
      config_.rate_hz = value;
      // Updates the LFO increment in place; preserves oscillator phase.
      lfos_[0].set_rate_hz(config_.rate_hz);
      lfos_[1].set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      if (!in_domain(value, kDepthMs)) return false;
      config_.depth_ms = value;
      return true;
    case 2:
      if (!in_domain(value, kCenterDelayMs)) return false;
      config_.center_delay_ms = value;
      return true;
    case 3:
      if (!in_domain(value, kDryWet)) return false;
      config_.dry_wet = value;
      return true;
    case 4: {
      config_.pre_filter_hz = value;
      // Re-derives the pole from the new corner and keeps the running sample, so
      // a moved corner does not put a step through the delay.
      for (auto& pre_filter : pre_filters_) {
        const float carried = pre_filter.state();
        pre_filter.prepare(config_.pre_filter_mode, config_.pre_filter_hz, sample_rate_);
        if (pre_filter.active()) {
          pre_filter.set_state(carried);
        }
      }
      return true;
    }
    case 5:
      if (!in_domain(value, kFeedback)) return false;
      config_.feedback = value;
      return true;
    case 6:
      if (!in_domain(value, kPhaseDeg)) return false;
      config_.phase_deg = value;
      lfos_[1].reset(lfos_[0].phase() + static_cast<double>(config_.phase_deg) / kDegreesPerTurnD);
      return true;
    case 7: {
      // An unnamed mode is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kPreFilterModeCount)) {
        return false;
      }
      config_.pre_filter_mode = static_cast<PreFilterMode>(static_cast<int>(value));
      for (auto& pre_filter : pre_filters_) {
        const float carried = pre_filter.state();
        pre_filter.prepare(config_.pre_filter_mode, config_.pre_filter_hz, sample_rate_);
        if (pre_filter.active()) {
          pre_filter.set_state(carried);
        }
      }
      return true;
    }
    case 8: {
      if (!delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<DelayInterpolation>(static_cast<int>(value));
      for (auto& delay : delays_) delay.set_interpolation(config_.interpolation);
      return true;
    }
    case 9:
      return common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

bool Chorus::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar/coefficient update; the
  // delay lines are pre-sized to the accepted range at prepare(), so no id
  // allocates or resets audio state. Unknown ids are rejected by set_parameter.
  return param_id <= 9;
}

std::vector<rt::ParamDescriptor> Chorus::parameter_descriptors() const {
  return {{"rateHz", 0},        {"depthMs", 1},  {"centerDelayMs", 2}, {"dryWet", 3},
          {"preFilterHz", 4},   {"feedback", 5}, {"phaseDeg", 6},      {"preFilterMode", 7},
          {"interpolation", 8}, {"mixLaw", 9}};
}

void Chorus::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  for (auto& pre_filter : pre_filters_) {
    pre_filter.reset();
  }
  for (auto& gate : pre_filter_gates_) gate.close();
  for (auto& filter : feedback_filters_) {
    filter.reset();
  }
  feedback_.fill(0.0f);
  lfos_[0].reset(0.0);
  lfos_[1].reset(static_cast<double>(config_.phase_deg) / kDegreesPerTurnD);
}

}  // namespace sonare::effects::modulation
