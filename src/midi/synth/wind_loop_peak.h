#pragma once

/// @file wind_loop_peak.h
/// @brief Peak gain of a wind bore's bell lowpass times its in-loop DC blocker.
///
/// The blocker's passband rises to 2/(1+r) at Nyquist, so a bore whose bell loss
/// sits near unity can carry more than unity round the loop. An engine caps its
/// bell loss gain at the reciprocal of this peak; the two forms are the two bell
/// filters the wind engines run.

#include <cmath>

namespace sonare::midi::synth {

/// Peak over frequency of the one-pole bell y += a(x - y) times the blocker (1 - z^-1)/(1 - r
/// z^-1). With u = 1 - cos(w) the squared product is u / ((A + B u)(C + D u)), whose maximum sits
/// at u = sqrt(AC/BD) when that is inside [0, 2] and at Nyquist otherwise.
inline double blocker_onepole_peak(double a, double r) noexcept {
  const double b = 1.0 - a;
  if (b <= 0.0 || a * (1.0 - r) >= 4.0 * std::sqrt(b * r)) return a / (2.0 - a) * 2.0 / (1.0 + r);
  return a / (a * std::sqrt(r) + (1.0 - r) * std::sqrt(b));
}

/// The same peak for the Butterworth bell prewarped at k = tan(w_c/2). With x = tan^2(w/2) the
/// blocker is 4x/((1+r)^2 (p^2 + x)), p = (1-r)/(1+r), and the bell 1/(1 + x^2/k^4); the product
/// peaks at the one positive root of 2x^3 + p^2 x^2 - p^2 k^4, which Newton reaches from above.
inline double blocker_butterworth_peak(double k, double r) noexcept {
  const double p2 = (1.0 - r) * (1.0 - r) / ((1.0 + r) * (1.0 + r));
  const double q2 = k * k * k * k;
  double x = std::cbrt(0.5 * p2 * q2);
  for (int i = 0; i < 64; ++i) {
    const double next = x - ((2.0 * x + p2) * x * x - p2 * q2) / ((6.0 * x + 2.0 * p2) * x);
    if (!(next < x)) break;
    x = next;
  }
  return 2.0 / (1.0 + r) * std::sqrt(x / ((p2 + x) * (1.0 + x * x / q2)));
}

}  // namespace sonare::midi::synth
