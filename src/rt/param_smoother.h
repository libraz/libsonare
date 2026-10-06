#pragma once

/// @file param_smoother.h
/// @brief One-pole parameter smoothing.

#include <atomic>

namespace sonare::rt {

class ParamSmoother {
 public:
  ParamSmoother() = default;
  ParamSmoother(float initial_value, float time_ms, double sample_rate);
  ParamSmoother(const ParamSmoother& other) noexcept;
  ParamSmoother& operator=(const ParamSmoother& other) noexcept;

  void prepare(double sample_rate, float time_ms);
  void reset(float value);
  void set_target(float value);
  float process();
  /// Like process(), but a glide whose step has rounded to zero a few ulps short of
  /// the target is finished at the target, so a settled value equals the one set.
  /// For position smoothers (pan, azimuth) whose exact value selects a code path.
  float process_settling();
  /// Advances the one-pole by @p n samples in closed form, equivalent to
  /// calling process() @p n times but without the per-sample loop. Returns the
  /// resulting current value. For @p n <= 0 the state is left unchanged.
  float advance(int n);

  float current() const { return current_; }
  float target() const { return target_.load(std::memory_order_acquire); }

 private:
  void update_coefficient();

  double sample_rate_ = 48000.0;
  float time_ms_ = 20.0f;
  float coefficient_ = 0.0f;
  float current_ = 0.0f;
  std::atomic<float> target_{0.0f};
};

}  // namespace sonare::rt
