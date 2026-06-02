#include "rt/param_smoother.h"

#include <algorithm>
#include <cmath>

#include "util/dsp_primitives.h"

namespace sonare::rt {

ParamSmoother::ParamSmoother(float initial_value, float time_ms, double sample_rate)
    : sample_rate_(sample_rate),
      time_ms_(time_ms),
      current_(initial_value),
      target_(initial_value) {
  update_coefficient();
}

void ParamSmoother::prepare(double sample_rate, float time_ms) {
  sample_rate_ = sample_rate;
  time_ms_ = time_ms;
  update_coefficient();
}

void ParamSmoother::reset(float value) {
  current_ = value;
  target_ = value;
}

void ParamSmoother::set_target(float value) { target_ = value; }

float ParamSmoother::process() {
  current_ += coefficient_ * (target_ - current_);
  return current_;
}

float ParamSmoother::advance(int n) {
  if (n <= 0) return current_;
  // Closed form of n iterations of current += coeff * (target - current):
  //   current = target + (current - target) * (1 - coeff)^n.
  const float decay = std::pow(1.0f - coefficient_, static_cast<float>(n));
  current_ = target_ + (current_ - target_) * decay;
  return current_;
}

void ParamSmoother::update_coefficient() {
  const float clamped_ms = std::max(time_ms_, 0.0f);
  coefficient_ = time_to_attack_release_rate_f(sample_rate_, clamped_ms);
}

}  // namespace sonare::rt
