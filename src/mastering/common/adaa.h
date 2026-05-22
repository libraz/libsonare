#pragma once

/// @file adaa.h
/// @brief First-order antiderivative antialiasing processor.

#include <cmath>

namespace sonare::mastering::common {

template <typename Nonlinearity>
class Adaa1 {
 public:
  explicit Adaa1(Nonlinearity nonlinearity = {}) : nonlinearity_(nonlinearity) {}

  float process(float x) noexcept {
    const float f1_x = nonlinearity_.antiderivative(x);
    const float dx = x - prev_x_;
    float y = 0.0f;
    if (std::abs(dx) > epsilon_) {
      y = (f1_x - prev_f1_) / dx;
    } else {
      y = nonlinearity_.apply(0.5f * (x + prev_x_));
    }
    prev_x_ = x;
    prev_f1_ = f1_x;
    return y;
  }

  void reset(float x = 0.0f) noexcept {
    prev_x_ = x;
    prev_f1_ = nonlinearity_.antiderivative(x);
  }

  int latency_samples() const noexcept { return 0; }
  int latency_samples_q8() const noexcept { return 128; }

 private:
  Nonlinearity nonlinearity_{};
  float prev_x_ = 0.0f;
  float prev_f1_ = nonlinearity_.antiderivative(0.0f);
  float epsilon_ = 1.0e-5f;
};

}  // namespace sonare::mastering::common
