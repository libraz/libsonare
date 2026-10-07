#pragma once

/// @file parameter_domain.h
/// @brief Accepted value domains shared by construction, chain overrides and live setters.

#include <cmath>

namespace sonare::mastering::common {

/// A true-peak ceiling is a finite level at or below full scale.
inline bool valid_ceiling_db(float ceiling) noexcept {
  return std::isfinite(ceiling) && ceiling <= 0.0f;
}

/// A switch carried as a number is exactly 0 or 1; NaN is neither.
inline bool valid_switch_value(double value) noexcept { return value == 0.0 || value == 1.0; }

}  // namespace sonare::mastering::common
