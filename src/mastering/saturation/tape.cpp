#include "mastering/saturation/tape.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::saturation {

namespace {

// Jiles-Atherton parameters that stay constant; the user-facing `saturation`
// and `hysteresis` knobs modulate `a_` and `k_` inside process_sample.
constexpr float kMs = 1.0f;        // saturation magnetization
constexpr float kAlpha = 1.6e-3f;  // inter-domain mean-field coupling
constexpr float kC = 0.4f;         // reversibility ratio in [0, 1]

}  // namespace

Tape::Tape(TapeConfig config) : config_(config) { validate_config(config_); }

void Tape::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) throw std::invalid_argument("sample_rate must be positive");
  if (max_block_size < 0) throw std::invalid_argument("max_block_size must be non-negative");
  prepared_ = true;
  reset();
}

void Tape::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_) throw std::logic_error("Tape must be prepared before processing");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  ensure_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
    auto& state = states_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = process_sample(state, channels[ch][i]);
    }
  }
}

void Tape::reset() {
  for (auto& s : states_) {
    s.M = 0.0f;
    s.H_prev = 0.0f;
  }
}

void Tape::set_config(const TapeConfig& config) {
  validate_config(config);
  config_ = config;
}

void Tape::validate_config(const TapeConfig& config) {
  if (config.saturation < 0.0f || config.saturation > 1.0f || config.hysteresis < 0.0f ||
      config.hysteresis > 1.0f) {
    throw std::invalid_argument("invalid tape configuration");
  }
}

float Tape::db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float Tape::langevin(float x) {
  // L(x) = coth(x) - 1/x. Taylor expansion near 0 avoids the 0/0 singularity.
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    // Truncated Maclaurin: L(x) = x/3 - x^3/45 + ...
    return x * (1.0f / 3.0f - x * x / 45.0f);
  }
  return 1.0f / std::tanh(x) - 1.0f / x;
}

float Tape::langevin_derivative(float x) {
  // L'(x) = 1/x^2 - csch^2(x). Taylor near 0: L'(x) = 1/3 - x^2/15 + ...
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    return 1.0f / 3.0f - x * x / 15.0f;
  }
  const float sinh_x = std::sinh(x);
  return 1.0f / (x * x) - 1.0f / (sinh_x * sinh_x);
}

float Tape::process_sample(JaState& state, float input) const {
  // Map user controls to J-A parameters:
  //   a (anhysteretic shape) is smaller when saturation is large → earlier saturation.
  //   k (loss/coercivity)    is larger when hysteresis is large → wider loop.
  const float a = std::max(0.05f, 0.5f - 0.4f * config_.saturation);
  const float k = std::max(0.01f, 0.05f + 0.30f * config_.hysteresis);

  const float drive = db_to_linear(config_.drive_db);
  const float H = input * drive;

  const float He = H + kAlpha * state.M;
  const float x = He / a;

  // Anhysteretic (lossless) magnetization curve.
  const float M_an = kMs * langevin(x);

  // Direction of field change. The irreversible component only updates when
  // moving in the direction that the bulk magnetization "wants" to follow.
  const float dH = H - state.H_prev;
  if (std::abs(dH) < 1e-9f) {
    state.H_prev = H;
    return state.M * db_to_linear(config_.output_gain_db);
  }
  const float delta = dH >= 0.0f ? 1.0f : -1.0f;

  // dM_irr/dH = (M_an - M) / (k*delta - alpha*(M_an - M)).
  const float diff = M_an - state.M;
  const float denom = k * delta - kAlpha * diff;
  float dM_irr_dH = 0.0f;
  if (std::abs(denom) > 1e-9f) {
    dM_irr_dH = diff / denom;
    // Suppress irreversible updates whose sign opposes the field direction —
    // this enforces the J-A "wiping out" property and prevents instability.
    if (dM_irr_dH * delta < 0.0f) dM_irr_dH = 0.0f;
  }

  // Reversible component follows the anhysteretic derivative.
  const float dL = langevin_derivative(x);
  const float dM_an_dHe = kMs * dL / a;
  // Chain rule through He = H + alpha*M; for small kAlpha this is ~ dM_an_dHe.
  const float dM_an_dH = dM_an_dHe / std::max(1.0f - kAlpha * dM_an_dHe, 1e-6f);

  const float dM_dH = (1.0f - kC) * dM_irr_dH + kC * dM_an_dH;

  // Forward-Euler integration. dH is bounded since input is in [-1, 1] and
  // drive is bounded, so a single step is stable for typical audio.
  state.M += dM_dH * dH;
  state.M = std::clamp(state.M, -1.2f * kMs, 1.2f * kMs);
  state.H_prev = H;

  return state.M * db_to_linear(config_.output_gain_db);
}

void Tape::ensure_state(int num_channels) {
  if (states_.size() != static_cast<size_t>(num_channels)) {
    states_.assign(static_cast<size_t>(num_channels), JaState{});
  }
}

}  // namespace sonare::mastering::saturation
