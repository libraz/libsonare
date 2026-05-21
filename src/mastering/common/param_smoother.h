#pragma once

/// @file param_smoother.h
/// @brief One-pole parameter smoothing.

namespace sonare::mastering::common {

class ParamSmoother {
 public:
  ParamSmoother() = default;
  ParamSmoother(float initial_value, float time_ms, double sample_rate);

  void prepare(double sample_rate, float time_ms);
  void reset(float value);
  void set_target(float value);
  float process();

  float current() const { return current_; }
  float target() const { return target_; }

 private:
  void update_coefficient();

  double sample_rate_ = 48000.0;
  float time_ms_ = 20.0f;
  float coefficient_ = 0.0f;
  float current_ = 0.0f;
  float target_ = 0.0f;
};

}  // namespace sonare::mastering::common
