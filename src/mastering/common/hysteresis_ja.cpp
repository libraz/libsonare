#include "mastering/common/hysteresis_ja.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare::mastering::common {

JilesAtherton::JilesAtherton(JilesAthertonConfig config) : config_(config) {
  validate_config(config_);
}

void JilesAtherton::set_config(const JilesAthertonConfig& config) {
  validate_config(config);
  config_ = config;
}

void JilesAtherton::integrate_step(JilesAthertonState& state, float field, float d_field) const {
  const float ms = config_.saturation_magnetization;
  const float alpha = config_.mean_field_coupling;
  const float a = config_.anhysteretic_shape;
  const float k = config_.coercivity;
  const float c = config_.reversibility;

  const float effective_field = field + alpha * state.magnetization;
  const float x = effective_field / a;
  const float anhysteretic = ms * langevin(x);

  const float delta = d_field >= 0.0f ? 1.0f : -1.0f;

  const float diff = anhysteretic - state.magnetization;
  const float delta_m = delta * diff >= 0.0f ? 1.0f : 0.0f;
  const float denom = (1.0f - c) * delta * k - alpha * diff;
  float d_m_hyst_d_h = 0.0f;
  if (std::abs(denom) > 1e-9f) {
    d_m_hyst_d_h = (1.0f - c) * delta_m * diff / denom;
  }

  const float d_l = langevin_derivative(x);
  const float d_m_an_d_he = ms * d_l / a;
  // Mean-field correction turning a derivative with respect to the effective
  // field into one with respect to the drive field.
  const float mean_field_gain = std::max(1.0f - alpha * d_m_an_d_he, 1e-6f);

  const float d_m_irr = d_m_hyst_d_h * d_field;

  // The reversible branch is integrated in closed form rather than by the same
  // Euler step, because the antiderivative of dMan/dH is the anhysteretic
  // magnetization itself. Both evaluations hold the magnetization fixed, so the
  // difference is with respect to the effective field and is divided by the mean
  // field gain to become one with respect to the drive field - the same
  // conversion the derivative form applies, and the term that sets the
  // small-field susceptibility.
  const float an_start = ms * langevin((field - d_field + alpha * state.magnetization) / a);
  const float d_m_rev = c * (anhysteretic - an_start) / mean_field_gain;

  // Both branches chase the anhysteretic curve, so their sum must not carry the
  // magnetization across it: the loop-transition slope is roughly diff/k and a
  // step of size d_field therefore overshoots as soon as d_field approaches k,
  // while the reversible term telescopes to c times the whole anhysteretic
  // excursion over a fast half cycle and so overshoots on its own even where the
  // irreversible term has already landed on the curve. Projecting the total
  // increment onto the remaining distance keeps the trajectory on the branch it
  // is on, which is what the sub-stepping above achieves while it has sub-steps
  // left to spend and what has to hold once the sub-step budget is exhausted.
  //
  // The distance projected onto is `diff` divided by the mean-field gain rather
  // than `diff` itself. `diff` is measured against the anhysteretic value for
  // the magnetization the step starts from, which is not yet self-consistent;
  // dividing by the gain is the first-order correction to the value the curve
  // actually settles at, and it is what keeps the projection from cancelling the
  // mean-field term the small-field susceptibility depends on.
  const float reach = diff / mean_field_gain;
  const float d_m =
      diff >= 0.0f ? std::min(d_m_irr + d_m_rev, reach) : std::max(d_m_irr + d_m_rev, reach);

  state.magnetization += d_m;
  state.magnetization = std::clamp(state.magnetization, -1.2f * ms, 1.2f * ms);
}

float JilesAtherton::process(JilesAthertonState& state, float field, float sample_rate) const {
  const float ms = config_.saturation_magnetization;
  const float alpha = config_.mean_field_coupling;
  const float a = config_.anhysteretic_shape;

  const float d_field = field - state.previous_field;
  if (std::abs(d_field) < 1e-9f) {
    const float x = (field + alpha * state.magnetization) / a;
    const float anhysteretic = ms * langevin(x);
    // Magnetic after-effect. The rate-independent loop equation is driven by
    // dM/dH, so with the field held constant there is nothing to step and the
    // magnetization would stay wherever the last field change left it - for a
    // transient large enough to hit the saturation clamp, permanently. Relax it
    // toward the anhysteretic curve instead, which is the equilibrium
    // magnetization for the held field.
    const float tau = config_.viscosity_time_constant_s;
    if (tau > 0.0f && sample_rate > 0.0f) {
      const float relax_rate = 1.0f - std::exp(-1.0f / (tau * sample_rate));
      state.magnetization += relax_rate * (anhysteretic - state.magnetization);
    }
    state.previous_field = field;
    return state.magnetization;
  }

  // The loop-transition slope is roughly diff/k, so a single Euler step of size
  // d_field moves the magnetization by about diff*(d_field/k) and overshoots the
  // anhysteretic target it is chasing once d_field approaches k. That is a slew
  // limit, not a level limit: it is reached by ordinary high-frequency content,
  // and the saturation clamp then holds the output at full scale independently
  // of input level. Splitting a large field change into sub-steps keeps each
  // step inside the stable range; the field is walked linearly to its new value
  // and the slope is re-evaluated at every sub-step.
  int sub_steps = 1;
  if (config_.max_field_step > 0.0f) {
    const float wanted = std::abs(d_field) / config_.max_field_step;
    sub_steps = std::min(kMaxSubSteps, 1 + static_cast<int>(wanted));
  }
  const float start_field = state.previous_field;
  const float sub_d_field = d_field / static_cast<float>(sub_steps);
  for (int i = 1; i <= sub_steps; ++i) {
    // The last sub-step lands on the requested field exactly, so a sub-stepped
    // run cannot drift away from the single-step one by accumulated rounding.
    const float sub_field =
        i == sub_steps ? field : start_field + sub_d_field * static_cast<float>(i);
    integrate_step(state, sub_field, sub_d_field);
  }
  state.previous_field = field;

  return state.magnetization;
}

void JilesAtherton::reset(JilesAthertonState& state) noexcept {
  state.magnetization = 0.0f;
  state.previous_field = 0.0f;
}

float JilesAtherton::langevin(float x) {
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    return x * (1.0f / 3.0f - x * x / 45.0f);
  }
  return 1.0f / std::tanh(x) - 1.0f / x;
}

float JilesAtherton::langevin_derivative(float x) {
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    return 1.0f / 3.0f - x * x / 15.0f;
  }
  const float sinh_x = std::sinh(x);
  return 1.0f / (x * x) - 1.0f / (sinh_x * sinh_x);
}

void JilesAtherton::validate_config(const JilesAthertonConfig& config) {
  if (!(config.saturation_magnetization > 0.0f) || !(config.anhysteretic_shape > 0.0f) ||
      !(config.coercivity > 0.0f) || config.mean_field_coupling < 0.0f ||
      config.reversibility < 0.0f || config.reversibility > 1.0f ||
      config.viscosity_time_constant_s < 0.0f || config.max_field_step < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid Jiles-Atherton configuration");
  }
}

namespace jiles_atherton_presets {

// Chowdhury, "Real-time Physical Modelling for Analog Tape Machines",
// DAFx-19 2019, section 2.2 equations (6)-(10), with Sony TC-260
// ferric oxide tape constants summarized in figure 6. Values here are
// normalized for audio-rate use while preserving the equation structure.

JilesAthertonConfig oxide_tape() { return {1.0f, 0.3f, 0.1f, 1.6e-3f, 0.4f}; }

JilesAthertonConfig tape() { return oxide_tape(); }

JilesAthertonConfig silicon_steel() {
  // A conservative audio transformer core preset: higher coercivity than the
  // tape default for a wider low-level loop, with a mostly reversible curve to
  // avoid excessive remanence in normal line-level operation.
  return {1.0f, 0.22f, 0.16f, 2.0e-3f, 0.55f};
}

JilesAthertonConfig mu_metal() {
  // High-permeability shielded transformer preset: narrower coercivity and
  // stronger reversible component than silicon steel.
  return {1.0f, 0.18f, 0.055f, 2.8e-3f, 0.72f};
}

}  // namespace jiles_atherton_presets

namespace presets {

JilesAthertonConfig oxide_tape() { return jiles_atherton_presets::oxide_tape(); }
JilesAthertonConfig tape() { return jiles_atherton_presets::tape(); }
JilesAthertonConfig silicon_steel() { return jiles_atherton_presets::silicon_steel(); }
JilesAthertonConfig mu_metal() { return jiles_atherton_presets::mu_metal(); }

}  // namespace presets

}  // namespace sonare::mastering::common
