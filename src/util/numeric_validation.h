#pragma once

/// @file numeric_validation.h
/// @brief Shared finite/range/float-to-integral validation at public boundaries.

#include <cmath>
#include <limits>
#include <type_traits>

namespace sonare::numeric {

template <typename Float>
inline bool finite(Float value) noexcept {
  static_assert(std::is_floating_point_v<Float>);
  return std::isfinite(value);
}

template <typename Float>
inline bool finite_positive(Float value) noexcept {
  return finite(value) && value > static_cast<Float>(0);
}

template <typename Float>
inline bool finite_non_negative(Float value) noexcept {
  return finite(value) && value >= static_cast<Float>(0);
}

template <typename Float>
inline bool finite_in_closed_range(Float value, Float minimum, Float maximum) noexcept {
  return finite(value) && value >= minimum && value <= maximum;
}

template <typename Float>
inline bool finite_ordered_range(Float minimum, Float maximum) noexcept {
  return finite(minimum) && finite(maximum) && minimum < maximum;
}

/// Exact floating-point to integral conversion. Fractional, non-finite and
/// out-of-representation-range values are rejected before any cast occurs.
template <typename Int, typename Float>
inline bool checked_integral_cast(Float value, Int* out) noexcept {
  static_assert(std::is_integral_v<Int> && std::is_floating_point_v<Float>);
  if (out == nullptr || !finite(value)) return false;
  const long double wide = static_cast<long double>(value);
  if (std::trunc(wide) != wide ||
      wide < static_cast<long double>(std::numeric_limits<Int>::lowest()) ||
      wide > static_cast<long double>(std::numeric_limits<Int>::max())) {
    return false;
  }
  *out = static_cast<Int>(value);
  return true;
}

/// Rounded floating-point to integral conversion with explicit bounds.
template <typename Int, typename Float>
inline bool checked_round_cast(Float value, Int* out) noexcept {
  static_assert(std::is_integral_v<Int> && std::is_floating_point_v<Float>);
  if (out == nullptr || !finite(value)) return false;
  const long double rounded = std::round(static_cast<long double>(value));
  if (rounded < static_cast<long double>(std::numeric_limits<Int>::lowest()) ||
      rounded > static_cast<long double>(std::numeric_limits<Int>::max())) {
    return false;
  }
  *out = static_cast<Int>(rounded);
  return true;
}

}  // namespace sonare::numeric
