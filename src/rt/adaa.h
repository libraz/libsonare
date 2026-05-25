#pragma once

/// @file adaa.h
/// @brief Antiderivative antialiasing processors (1st and 2nd order).

#include <cmath>
#include <type_traits>
#include <utility>

namespace sonare::rt {

// Divided-difference denominator threshold for the ADAA recurrences. This is an
// algorithm-specific guard against ill-conditioned division when consecutive
// samples are nearly equal, intentionally distinct from constants::kEpsilon
// (1e-10) which is far too small to keep these ratios numerically stable.
constexpr float kAdaaDivisorEpsilon = 1.0e-5f;

template <typename Nonlinearity>
class Adaa1 {
 public:
  explicit Adaa1(Nonlinearity nonlinearity = {}) : nonlinearity_(nonlinearity) {}

  float process(float x) noexcept {
    const float f1_x = nonlinearity_.antiderivative(x);
    const float dx = x - prev_x_;
    float y = 0.0f;
    if (std::abs(dx) > kEpsilon_) {
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
  static constexpr float kEpsilon_ = kAdaaDivisorEpsilon;
};

/// @brief Detects whether a nonlinearity provides a second antiderivative (F2).
template <typename N, typename = void>
struct has_second_antiderivative : std::false_type {};
template <typename N>
struct has_second_antiderivative<
    N, std::void_t<decltype(std::declval<N>().second_antiderivative(0.0f))>> : std::true_type {};

/// @brief Second-order antiderivative antialiasing processor.
/// @details Uses the second divided difference of the second antiderivative (F2)
///   of the nonlinearity, expressed in a numerically robust form to avoid the
///   singularities of the naive closed form when sample differences are small.
///   Reports one sample of latency.
template <typename Nonlinearity>
class Adaa2 {
  static_assert(has_second_antiderivative<Nonlinearity>::value,
                "Adaa2 requires second_antiderivative()");

 public:
  explicit Adaa2(Nonlinearity nonlinearity = {}) : nonlinearity_(nonlinearity) { reset(); }

  float process(float x0) noexcept {
    const float f1_x0 = nonlinearity_.antiderivative(x0);
    const float f2_x0 = nonlinearity_.second_antiderivative(x0);
    const float d02 = x0 - prev_x2_;
    const float d01 = x0 - prev_x1_;
    const float d12 = prev_x1_ - prev_x2_;

    float y = 0.0f;
    if (std::abs(d02) >= kEps) {
      // Case 1: outer samples are distinct; standard second divided difference.
      const float d1_01 = (std::abs(d01) >= kEps)
                              ? (f2_x0 - prev_f2_x1_) / d01
                              : nonlinearity_.antiderivative(0.5f * (x0 + prev_x1_));
      const float d1_12 = (std::abs(d12) >= kEps)
                              ? (prev_f2_x1_ - prev_f2_x2_) / d12
                              : nonlinearity_.antiderivative(0.5f * (prev_x1_ + prev_x2_));
      y = 2.0f * (d1_01 - d1_12) / d02;
    } else if (std::abs(d01) >= kEps) {
      // Case 2b: x[n-2] ~= x[n], limit of the second divided difference.
      // [F2; x0, x0, x1] = (F1(x0) * d01 - F2(x0) + F2(x1)) / d01^2.
      y = 2.0f * (f1_x0 * d01 - f2_x0 + prev_f2_x1_) / (d01 * d01);
    } else {
      // Case 2a: all three samples coincide; evaluate at the center sample.
      y = nonlinearity_.apply(prev_x1_);
    }

    prev_x2_ = prev_x1_;
    prev_f2_x2_ = prev_f2_x1_;
    prev_x1_ = x0;
    prev_f2_x1_ = f2_x0;
    return y;
  }

  void reset(float x = 0.0f) noexcept {
    prev_x1_ = x;
    prev_x2_ = x;
    prev_f2_x1_ = nonlinearity_.second_antiderivative(x);
    prev_f2_x2_ = prev_f2_x1_;
  }

  int latency_samples() const noexcept { return 1; }
  int latency_samples_q8() const noexcept { return 256; }

 private:
  static constexpr float kEps = kAdaaDivisorEpsilon;

  Nonlinearity nonlinearity_{};
  float prev_x1_ = 0.0f;
  float prev_x2_ = 0.0f;
  float prev_f2_x1_ = 0.0f;
  float prev_f2_x2_ = 0.0f;
};

}  // namespace sonare::rt
