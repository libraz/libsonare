#pragma once

/// @file numeric_validation.h
/// @brief Shared finite/range/float-to-integral validation at public boundaries.

#include <cmath>
#include <cstddef>
#include <cstdint>
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

/// Checked floating-point narrowing. Non-finite and target-range-exceeding
/// values are rejected before the cast; ordinary rounding within the target
/// type's finite range is permitted.
template <typename To, typename From>
inline bool checked_float_cast(From value, To* out) noexcept {
  static_assert(std::is_floating_point_v<To> && std::is_floating_point_v<From>);
  if (out == nullptr || !finite(value)) return false;
  const long double wide = static_cast<long double>(value);
  if (wide < static_cast<long double>(std::numeric_limits<To>::lowest()) ||
      wide > static_cast<long double>(std::numeric_limits<To>::max())) {
    return false;
  }
  *out = static_cast<To>(value);
  return true;
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

/// Multiplies two element counts without overflowing and enforces an explicit
/// resource limit. `out` is only written on success.
inline bool checked_size_product(std::size_t lhs, std::size_t rhs, std::size_t limit,
                                 std::size_t* out) noexcept {
  if (out == nullptr || (rhs != 0 && lhs > limit / rhs)) return false;
  const std::size_t product = lhs * rhs;
  if (product > limit) return false;
  *out = product;
  return true;
}

/// Adds two integral values without overflowing. `out` is written only on
/// success.
template <typename Int>
inline bool checked_add(Int lhs, Int rhs, Int* out) noexcept {
  static_assert(std::is_integral_v<Int>);
  if (out == nullptr) return false;
  if constexpr (std::is_signed_v<Int>) {
    if ((rhs > 0 && lhs > std::numeric_limits<Int>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<Int>::lowest() - rhs)) {
      return false;
    }
  } else if (lhs > std::numeric_limits<Int>::max() - rhs) {
    return false;
  }
  *out = static_cast<Int>(lhs + rhs);
  return true;
}

/// Saturating integral addition for timeline/sample arithmetic that cannot
/// report an error from a realtime path.
template <typename Int>
inline Int saturating_add(Int lhs, Int rhs) noexcept {
  static_assert(std::is_integral_v<Int> && std::is_signed_v<Int>);
  Int result = 0;
  if (checked_add(lhs, rhs, &result)) return result;
  return rhs >= 0 ? std::numeric_limits<Int>::max() : std::numeric_limits<Int>::lowest();
}

/// Computes ceil(input_count / rate) without an undefined floating-point to
/// integer cast. Non-finite/non-positive rates and results above `limit` are
/// rejected. `long double` keeps the boundary comparison stable for large
/// `size_t` inputs.
template <typename Float>
inline bool checked_projected_count(std::size_t input_count, Float rate, std::size_t limit,
                                    std::size_t* out) noexcept {
  static_assert(std::is_floating_point_v<Float>);
  if (out == nullptr || !finite_positive(rate)) return false;
  const long double projected =
      std::ceil(static_cast<long double>(input_count) / static_cast<long double>(rate));
  if (!std::isfinite(projected) || projected < 0.0L ||
      projected > static_cast<long double>(limit)) {
    return false;
  }
  *out = static_cast<std::size_t>(projected);
  return true;
}

/// Computes ceil(numerator / denominator) as a bounded size_t without a
/// floating-to-integral overflow. Useful for iteration counts where both terms
/// are floating-point timeline durations.
template <typename Float>
inline bool checked_ceil_ratio(Float numerator, Float denominator, std::size_t limit,
                               std::size_t* out) noexcept {
  static_assert(std::is_floating_point_v<Float>);
  if (out == nullptr || !finite_non_negative(numerator) || !finite_positive(denominator)) {
    return false;
  }
  const long double projected =
      std::ceil(static_cast<long double>(numerator) / static_cast<long double>(denominator));
  if (!std::isfinite(projected) || projected < 0.0L ||
      projected > static_cast<long double>(limit)) {
    return false;
  }
  *out = static_cast<std::size_t>(projected);
  return true;
}

}  // namespace sonare::numeric
