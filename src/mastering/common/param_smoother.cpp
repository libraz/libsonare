#include "mastering/common/param_smoother.h"

#include <algorithm>
#include <cmath>

namespace sonare::mastering::common {

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

void ParamSmoother::update_coefficient() {
  const float clamped_ms = std::max(time_ms_, 0.0f);
  if (clamped_ms == 0.0f || sample_rate_ <= 0.0) {
    coefficient_ = 1.0f;
    return;
  }

  const double samples = sample_rate_ * static_cast<double>(clamped_ms) * 0.001;
  coefficient_ = static_cast<float>(1.0 - std::exp(-1.0 / samples));
}

}  // namespace sonare::mastering::common
