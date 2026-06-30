#pragma once

/// @file bessel.h
/// @brief Bessel function of the first kind J_m(x) for the membrane
///        strike-point weighting (percussion_voice.h, synthesis method (6)).
///
/// libc++ (the default macOS/clang standard library) ships no
/// std::cyl_bessel_j, so the percussion core cannot depend on the C++17
/// special-math functions. This ascending-series evaluator covers the small
/// integer orders (m <= 3) and bounded arguments (|x| <~ 7) the circular
/// membrane needs. It is called only at note-on (never per-sample), so the
/// fixed term count is allocation-free and RT-safe.

#include <cmath>

namespace sonare::midi::synth {

/// @brief Bessel function of the first kind, integer order, via the ascending
///        power series J_m(x) = sum_k (-1)^k / (k! (k+m)!) (x/2)^(2k+m).
/// @param m Non-negative integer order (negative orders use |m|; the magnitude
///          is what the strike weighting consumes).
/// @param x Argument (radians of the membrane radial coordinate).
/// @return J_m(x).
inline float bessel_j(int m, float x) noexcept {
  if (m < 0) m = -m;
  const double half = 0.5 * static_cast<double>(x);
  const double half_sq = half * half;
  // term(k=0) = (x/2)^m / m!
  double term = 1.0;
  for (int i = 1; i <= m; ++i) term *= half / static_cast<double>(i);
  double sum = term;
  // Recurrence: term_k = term_{k-1} * -(x/2)^2 / (k (k+m)).
  for (int k = 1; k <= 24; ++k) {
    term *= -half_sq / (static_cast<double>(k) * static_cast<double>(k + m));
    sum += term;
    if (std::abs(term) < 1.0e-12 * std::abs(sum)) break;
  }
  return static_cast<float>(sum);
}

}  // namespace sonare::midi::synth
