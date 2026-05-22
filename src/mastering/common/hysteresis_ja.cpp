#include "mastering/common/hysteresis_ja.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::common {

JilesAtherton::JilesAtherton(JilesAthertonConfig config) : config_(config) {
  validate_config(config_);
}

void JilesAtherton::set_config(const JilesAthertonConfig& config) {
  validate_config(config);
  config_ = config;
}

float JilesAtherton::process(JilesAthertonState& state, float field) const {
  const float ms = config_.saturation_magnetization;
  const float alpha = config_.mean_field_coupling;
  const float a = config_.anhysteretic_shape;
  const float k = config_.coercivity;
  const float c = config_.reversibility;

  const float effective_field = field + alpha * state.magnetization;
  const float x = effective_field / a;

  const float anhysteretic = ms * langevin(x);
  const float d_field = field - state.previous_field;
  if (std::abs(d_field) < 1e-9f) {
    state.previous_field = field;
    return state.magnetization;
  }
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
  const float d_m_an_d_h = d_m_an_d_he / std::max(1.0f - alpha * d_m_an_d_he, 1e-6f);

  const float d_m_d_h = d_m_hyst_d_h + c * d_m_an_d_h;
  state.magnetization += d_m_d_h * d_field;
  state.magnetization = std::clamp(state.magnetization, -1.2f * ms, 1.2f * ms);
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
      config.reversibility < 0.0f || config.reversibility > 1.0f) {
    throw std::invalid_argument("invalid Jiles-Atherton configuration");
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
