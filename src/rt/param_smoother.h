#pragma once

/// @file param_smoother.h
/// @brief One-pole parameter smoothing.

#include <atomic>

#include "util/constants.h"

namespace sonare::rt {

/// One step of a first-order glide from @p current toward @p target, held in double. A step
/// that rounds to nothing finishes on the target, so a settled glide equals its target at
/// every sample rate instead of stalling a rate-dependent distance short of it.
inline double glide_toward(double current, double target, double coeff) noexcept {
  const double next = current + coeff * (target - current);
  return next == current ? target : next;
}

/// The same step for state kept in float. It settles exactly too, but finishes from up to half
/// a float ulp over the coefficient away, so a long glide belongs on the double form.
inline float glide_toward_f(float current, float target, float coeff) noexcept {
  const float next = current + coeff * (target - current);
  return next == current ? target : next;
}

class ParamSmoother {
 public:
  ParamSmoother() = default;
  ParamSmoother(float initial_value, float time_ms, double sample_rate);
  ParamSmoother(const ParamSmoother& other) noexcept;
  ParamSmoother& operator=(const ParamSmoother& other) noexcept;

  void prepare(double sample_rate, float time_ms);
  void reset(float value);
  void set_target(float value);
  /// One sample of the glide (see glide_toward): a settled value equals the one set.
  float process();
  /// Like process(), but finishes at the target once within @p epsilon of it,
  /// so a glide to 0 or 1 lands on the exact value instead of approaching it.
  float process_snapping(float epsilon);
  /// Advances the one-pole by @p n samples in closed form, equivalent to
  /// calling process() @p n times but without the per-sample loop. Returns the
  /// resulting current value. For @p n <= 0 the state is left unchanged.
  float advance(int n);

  float current() const { return static_cast<float>(current_); }
  float target() const { return target_.load(std::memory_order_acquire); }

 private:
  void update_coefficient();

  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  float time_ms_ = 20.0f;
  double coefficient_ = 0.0;
  double current_ = 0.0;
  std::atomic<float> target_{0.0f};
};

}  // namespace sonare::rt
