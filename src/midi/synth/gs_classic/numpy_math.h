#pragma once

/// @file numpy_math.h
/// @brief Numpy-compatible scalar helpers shared by the classic node renderers.

#include <cmath>

namespace sonare::midi::synth::gs_classic {

/// numpy's float remainder: the sign of the divisor, and +0 for an exact multiple.
inline double py_mod(double a, double b) noexcept {
  double m = std::fmod(a, b);
  if (m == 0.0) return std::copysign(0.0, b);
  if ((b < 0.0) != (m < 0.0)) m += b;
  return m;
}

}  // namespace sonare::midi::synth::gs_classic
