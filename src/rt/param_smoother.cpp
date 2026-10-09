#include "rt/param_smoother.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::rt {

using constants::kDefaultDawSampleRate;

ParamSmoother::ParamSmoother(float initial_value, float time_ms, double sample_rate)
    : sample_rate_(std::isfinite(sample_rate) && sample_rate > 0.0 ? sample_rate
                                                                   : kDefaultDawSampleRate),
      time_ms_(std::isfinite(time_ms) && time_ms >= 0.0f ? time_ms : 0.0f),
      current_(std::isfinite(initial_value) ? initial_value : 0.0f),
      target_(std::isfinite(initial_value) ? initial_value : 0.0f) {
  update_coefficient();
}

ParamSmoother::ParamSmoother(const ParamSmoother& other) noexcept
    : sample_rate_(other.sample_rate_),
      time_ms_(other.time_ms_),
      coefficient_(other.coefficient_),
      current_(other.current_),
      target_(other.target_.load(std::memory_order_relaxed)) {}

ParamSmoother& ParamSmoother::operator=(const ParamSmoother& other) noexcept {
  if (this == &other) return *this;
  sample_rate_ = other.sample_rate_;
  time_ms_ = other.time_ms_;
  coefficient_ = other.coefficient_;
  current_ = other.current_;
  target_.store(other.target_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  return *this;
}

void ParamSmoother::prepare(double sample_rate, float time_ms) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || !std::isfinite(time_ms)) return;
  sample_rate_ = sample_rate;
  time_ms_ = time_ms;
  update_coefficient();
}

void ParamSmoother::reset(float value) {
  if (!std::isfinite(value)) return;
  current_ = value;
  target_.store(value, std::memory_order_release);
}

void ParamSmoother::set_target(float value) {
  if (!std::isfinite(value)) return;
  target_.store(value, std::memory_order_release);
}

float ParamSmoother::process() {
  current_ = glide_toward(current_, target_.load(std::memory_order_acquire), coefficient_);
  return static_cast<float>(current_);
}

float ParamSmoother::advance(int n) {
  if (n <= 0) return static_cast<float>(current_);
  // Closed form of n iterations of current += coeff * (target - current):
  //   current = target + (current - target) * (1 - coeff)^n.
  const double target = target_.load(std::memory_order_acquire);
  const double decay = std::pow(1.0 - coefficient_, static_cast<double>(n));
  current_ = target + (current_ - target) * decay;
  return static_cast<float>(current_);
}

void ParamSmoother::update_coefficient() {
  const float clamped_ms = std::max(time_ms_, 0.0f);
  coefficient_ = time_to_attack_release_rate(sample_rate_, clamped_ms);
}

float ParamSmoother::process_snapping(float epsilon) {
  const double target = target_.load(std::memory_order_acquire);
  const double next = glide_toward(current_, target, coefficient_);
  current_ = std::abs(target - next) <= static_cast<double>(epsilon) ? target : next;
  return static_cast<float>(current_);
}

}  // namespace sonare::rt
