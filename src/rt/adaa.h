#pragma once

/// @file adaa.h
/// @brief Antiderivative antialiasing processors (1st and 2nd order).

#include <cmath>
#include <type_traits>
#include <utility>

#include "rt/aliasing_control.h"
#include "rt/tail_budget.h"

namespace sonare::rt {

// Divided-difference denominator threshold for the ADAA recurrences. This is an
// algorithm-specific guard against ill-conditioned division when consecutive
// samples are nearly equal, intentionally distinct from constants::kEpsilon
// (1e-10) which is far too small to keep these ratios numerically stable.
constexpr float kAdaaDivisorEpsilon = 1.0e-5f;

// Group delay of each order in Q8 samples: the first divided difference is
// centred half a sample back, the second a whole sample back.
constexpr int kAdaa1LatencySamplesQ8 = 128;
constexpr int kAdaa2LatencySamplesQ8 = 256;

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

namespace detail {

/// The one antiderivative evaluator the ADAA processors share: widened, from the shape's own
/// double accessor when it has one, so a divided difference never subtracts float-rounded
/// primitives.
template <typename N>
double antiderivative_at(N& nonlinearity, double x) noexcept {
  if constexpr (has_double_antiderivative<N>::value) {
    return nonlinearity.antiderivative_double(x);
  } else {
    return static_cast<double>(nonlinearity.antiderivative(static_cast<float>(x)));
  }
}

template <typename N>
double second_antiderivative_at(N& nonlinearity, double x) noexcept {
  if constexpr (has_double_second_antiderivative<N>::value) {
    return nonlinearity.second_antiderivative_double(x);
  } else {
    return static_cast<double>(nonlinearity.second_antiderivative(static_cast<float>(x)));
  }
}

}  // namespace detail

/// @brief First-order antiderivative antialiasing processor.
/// @details The divided difference is formed in double from one consistent curve: the float
///   primitives of a saturating shape carry an absolute rounding floor that, divided by a
///   sample step as small as the guard, exceeds the transfer's own bound.
template <typename Nonlinearity>
class Adaa1 {
 public:
  explicit Adaa1(Nonlinearity nonlinearity = {}) : nonlinearity_(nonlinearity) { reset(); }

  float process(float x) noexcept {
    const double x_d = static_cast<double>(x);
    const double f1_x = detail::antiderivative_at(nonlinearity_, x_d);
    const double dx = x_d - static_cast<double>(prev_x_);
    float y = 0.0f;
    if (std::abs(dx) > kEpsilon_) {
      y = static_cast<float>((f1_x - prev_f1_) / dx);
    } else {
      y = nonlinearity_.apply(0.5f * (x + prev_x_));
    }
    prev_x_ = x;
    prev_f1_ = f1_x;
    return y;
  }

  void reset(float x = 0.0f) noexcept {
    prev_x_ = x;
    prev_f1_ = detail::antiderivative_at(nonlinearity_, static_cast<double>(x));
  }

  /// @brief Replaces the wrapped nonlinearity for a shape parameter that moves at runtime.
  /// @details The stored previous primitive is re-evaluated with the new shape, so the next
  ///   divided difference spans one curve rather than two antiderivatives whose integration
  ///   constants differ.
  void set_nonlinearity(const Nonlinearity& nonlinearity) noexcept {
    nonlinearity_ = nonlinearity;
    prev_f1_ = detail::antiderivative_at(nonlinearity_, static_cast<double>(prev_x_));
  }

  static constexpr int kLatencySamplesQ8 = kAdaa1LatencySamplesQ8;
  int latency_samples_q8() const noexcept { return kLatencySamplesQ8; }
  int latency_samples() const noexcept { return kLatencySamplesQ8 >> 8; }

 private:
  Nonlinearity nonlinearity_{};
  float prev_x_ = 0.0f;
  double prev_f1_ = 0.0;
  static constexpr float kEpsilon_ = kAdaaDivisorEpsilon;
};

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
    const double f1_x0 = detail::antiderivative_at(nonlinearity_, x0_d);
    const double f2_x0 = detail::second_antiderivative_at(nonlinearity_, x0_d);
    const double d02 = x0_d - prev_x2_;
    const double d01 = x0_d - prev_x1_;
    const double d12 = prev_x1_ - prev_x2_;

    double y = 0.0;
    if (std::abs(d02) >= kEps) {
      // Case 1: outer samples are distinct; standard second divided difference.
      const double d1_01 = (std::abs(d01) >= kEps)
                               ? (f2_x0 - prev_f2_x1_) / d01
                               : detail::antiderivative_at(nonlinearity_, 0.5 * (x0_d + prev_x1_));
      const double d1_12 =
          (std::abs(d12) >= kEps)
              ? (prev_f2_x1_ - prev_f2_x2_) / d12
              : detail::antiderivative_at(nonlinearity_, 0.5 * (prev_x1_ + prev_x2_));
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
    prev_f2_x1_ = detail::second_antiderivative_at(nonlinearity_, static_cast<double>(x));
    prev_f2_x2_ = prev_f2_x1_;
  }

  static constexpr int kLatencySamplesQ8 = kAdaa2LatencySamplesQ8;
  int latency_samples() const noexcept { return kLatencySamplesQ8 >> 8; }
  int latency_samples_q8() const noexcept { return kLatencySamplesQ8; }

 private:
  static constexpr double kEps = kAdaaDivisorEpsilon;

  Nonlinearity nonlinearity_{};
  double prev_x1_ = 0.0;
  double prev_x2_ = 0.0;
  double prev_f2_x1_ = 0.0;
  double prev_f2_x2_ = 0.0;
};

/// @brief Q8 latency a nonlinear stage adds under @p mode at the host rate.
/// @param oversampled_round_trip_samples The stage's oversampler streaming round
///   trip in host samples, which only Oversample4x adds.
constexpr int aliasing_latency_samples_q8(AliasingControl mode,
                                          int oversampled_round_trip_samples) noexcept {
  switch (mode) {
    case AliasingControl::Adaa1:
      return kAdaa1LatencySamplesQ8;
    case AliasingControl::Adaa2:
      return kAdaa2LatencySamplesQ8;
    case AliasingControl::Oversample4x:
      return oversampled_round_trip_samples << 8;
    case AliasingControl::None:
      break;
  }
  return 0;
}

/// @brief Tail a nonlinear stage leaves under @p mode after its input stops: the
///        inputs ADAA remembers, or the oversampler's FIR ringing past its latency.
inline TailBudget aliasing_tail(AliasingControl mode, int oversampled_round_trip_samples) noexcept {
  TailBudget tail;
  switch (mode) {
    case AliasingControl::Adaa1:
      return tail.delay(1.0);
    case AliasingControl::Adaa2:
      return tail.delay(2.0);
    case AliasingControl::Oversample4x:
      return tail.delay(oversampled_round_trip_samples);
    case AliasingControl::None:
      break;
  }
  return tail;
}

}  // namespace sonare::rt
