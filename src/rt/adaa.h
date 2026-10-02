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

  /// @brief The wrapped nonlinearity, for shape parameters that move at runtime.
  /// @details Mutating it does not resync @c prev_f1_, which was taken with the
  ///   previous shape, so one divided difference straddles the change. That is
  ///   the same one-sample transient any parameter change through this processor
  ///   causes, and it is why callers should keep such a parameter smoothed or
  ///   slow rather than stepping it per block.
  Nonlinearity& nonlinearity() noexcept { return nonlinearity_; }
  const Nonlinearity& nonlinearity() const noexcept { return nonlinearity_; }

  // ADAA1 introduces a half-sample group delay (the first divided difference is
  // centered between x[n-1] and x[n]). 128 in Q8 is exactly 0.5 sample; the
  // integer accessor is derived from it (128 >> 8 == 0) so the two views stay
  // consistent instead of being independently hardcoded.
  int latency_samples_q8() const noexcept { return 128; }
  int latency_samples() const noexcept { return latency_samples_q8() >> 8; }

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

/// @brief Detects an optional widened antiderivative supplied by built-in shapes.
template <typename N, typename = void>
struct has_double_antiderivative : std::false_type {};
template <typename N>
struct has_double_antiderivative<
    N, std::void_t<decltype(std::declval<N&>().antiderivative_double(0.0))>> : std::true_type {};

/// @brief Detects an optional widened second antiderivative supplied by built-in shapes.
template <typename N, typename = void>
struct has_double_second_antiderivative : std::false_type {};
template <typename N>
struct has_double_second_antiderivative<
    N, std::void_t<decltype(std::declval<N&>().second_antiderivative_double(0.0))>>
    : std::true_type {};

/// @brief Second-order antiderivative antialiasing processor.
/// @details Uses the second divided difference of the second antiderivative (F2)
///   of the nonlinearity, expressed in a numerically robust form to avoid the
///   singularities of the naive closed form when sample differences are small.
///   The recurrence is evaluated in double precision: F2 values for a hard
///   clipper are around 1e-1 while their difference near a turning point is
///   around 1e-6, so storing them as float before the divided difference turns
///   quantization into audible impulses. Reports one sample of latency.
template <typename Nonlinearity>
class Adaa2 {
  static_assert(has_second_antiderivative<Nonlinearity>::value,
                "Adaa2 requires second_antiderivative()");

 public:
  explicit Adaa2(Nonlinearity nonlinearity = {}) : nonlinearity_(nonlinearity) { reset(); }

  float process(float x0) noexcept {
    // Shapes without *_double helpers are widened after evaluation.
    const double x0_d = static_cast<double>(x0);
    const double f1_x0 = antiderivative_at(nonlinearity_, x0_d);
    const double f2_x0 = second_antiderivative_at(nonlinearity_, x0_d);
    const double d02 = x0_d - prev_x2_;
    const double d01 = x0_d - prev_x1_;
    const double d12 = prev_x1_ - prev_x2_;

    double y = 0.0;
    if (std::abs(d02) >= kEps) {
      // Case 1: outer samples are distinct; standard second divided difference.
      const double d1_01 = (std::abs(d01) >= kEps)
                               ? (f2_x0 - prev_f2_x1_) / d01
                               : antiderivative_at(nonlinearity_, 0.5 * (x0_d + prev_x1_));
      const double d1_12 = (std::abs(d12) >= kEps)
                               ? (prev_f2_x1_ - prev_f2_x2_) / d12
                               : antiderivative_at(nonlinearity_, 0.5 * (prev_x1_ + prev_x2_));
      y = 2.0 * (d1_01 - d1_12) / d02;
    } else if (std::abs(d01) >= kEps) {
      // Case 2b: x[n-2] ~= x[n], limit of the second divided difference.
      // [F2; x0, x0, x1] = (F1(x0) * d01 - F2(x0) + F2(x1)) / d01^2.
      y = 2.0 * (f1_x0 * d01 - f2_x0 + prev_f2_x1_) / (d01 * d01);
    } else {
      // Case 2a: all three samples coincide; evaluate at the center sample.
      y = static_cast<double>(nonlinearity_.apply(static_cast<float>(prev_x1_)));
    }

    prev_x2_ = prev_x1_;
    prev_f2_x2_ = prev_f2_x1_;
    prev_x1_ = x0;
    prev_f2_x1_ = f2_x0;
    return static_cast<float>(y);
  }

  void reset(float x = 0.0f) noexcept {
    prev_x1_ = x;
    prev_x2_ = x;
    prev_f2_x1_ = second_antiderivative_at(nonlinearity_, static_cast<double>(x));
    prev_f2_x2_ = prev_f2_x1_;
  }

  int latency_samples() const noexcept { return 1; }
  int latency_samples_q8() const noexcept { return 256; }

 private:
  static double antiderivative_at(Nonlinearity& nonlinearity, double x) noexcept {
    if constexpr (has_double_antiderivative<Nonlinearity>::value) {
      return nonlinearity.antiderivative_double(x);
    } else {
      return static_cast<double>(nonlinearity.antiderivative(x));
    }
  }

  static double second_antiderivative_at(Nonlinearity& nonlinearity, double x) noexcept {
    if constexpr (has_double_second_antiderivative<Nonlinearity>::value) {
      return nonlinearity.second_antiderivative_double(x);
    } else {
      return static_cast<double>(nonlinearity.second_antiderivative(x));
    }
  }

  static constexpr double kEps = kAdaaDivisorEpsilon;

  Nonlinearity nonlinearity_{};
  double prev_x1_ = 0.0;
  double prev_x2_ = 0.0;
  double prev_f2_x1_ = 0.0;
  double prev_f2_x2_ = 0.0;
};

}  // namespace sonare::rt
