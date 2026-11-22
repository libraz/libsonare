#pragma once

/// @file db.h
/// @brief Scalar decibel conversion helpers.

#include <cmath>
#include <limits>

#include "util/constants.h"

namespace sonare {

/// Largest gain in dB a gain field accepts: a round bound inside the float range of
/// db_to_linear (finite up to about 770.6 dB), so it does not move with the platform's libm.
inline constexpr float kMaxFiniteGainDb = 770.0f;

/// Smallest gain in dB a field needing a positive linear gain accepts: a round bound above the
/// point where db_to_linear underflows to 0 (about -897 dB, through the subnormal range).
inline constexpr float kMinPositiveGainDb = -890.0f;

inline float db_to_linear(float db) noexcept { return std::pow(10.0f, db / 20.0f); }

inline double db_to_linear(double db) noexcept { return std::pow(10.0, db / 20.0); }

inline float db_to_power_scalar(float db) noexcept { return std::pow(10.0f, db / 10.0f); }

inline double db_to_power_scalar(double db) noexcept { return std::pow(10.0, db / 10.0); }

inline float linear_to_db(float value) noexcept {
  return value <= 0.0f ? constants::kFloorDb : 20.0f * std::log10(value);
}

inline double linear_to_db(double value) noexcept {
  return value <= 0.0 ? constants::kFloorDbD : 20.0 * std::log10(value);
}

inline float power_to_db_scalar(float power) noexcept {
  return power <= 0.0f ? constants::kFloorDb : 10.0f * std::log10(power);
}

inline double power_to_db_scalar(double power) noexcept {
  return power <= 0.0 ? constants::kFloorDbD : 10.0 * std::log10(power);
}

/// A measured level of zero energy: no measurable level, distinct from a level not measured.
inline constexpr float kNoLevelDb = -std::numeric_limits<float>::infinity();
inline constexpr double kNoLevelDbD = -std::numeric_limits<double>::infinity();
/// A level that was not measured (switched off, no frame, an undefined difference).
inline constexpr float kNotMeasured = std::numeric_limits<float>::quiet_NaN();
inline constexpr double kNotMeasuredD = std::numeric_limits<double>::quiet_NaN();

/// Measured level of a magnitude: the log for a positive value, kNoLevelDb for exact zero, NaN
/// passed through. Unlike linear_to_db there is no floor, so the reading is monotonic.
inline float amplitude_to_level_db(float value) noexcept {
  return value == 0.0f ? kNoLevelDb : 20.0f * std::log10(value);
}

inline double amplitude_to_level_db(double value) noexcept {
  return value == 0.0 ? kNoLevelDbD : 20.0 * std::log10(value);
}

/// Measured level of a power or mean square, on the same terms as amplitude_to_level_db.
inline float power_to_level_db(float power) noexcept {
  return power == 0.0f ? kNoLevelDb : 10.0f * std::log10(power);
}

inline double power_to_level_db(double power) noexcept {
  return power == 0.0 ? kNoLevelDbD : 10.0 * std::log10(power);
}

/// Difference of two levels: the result when finite or -inf, kNotMeasured when it would be NaN
/// or +inf (silence against silence, a finite level against silence).
inline float level_difference_db(float a, float b) noexcept {
  const float d = a - b;
  return std::isnan(d) || d == std::numeric_limits<float>::infinity() ? kNotMeasured : d;
}

inline double level_difference_db(double a, double b) noexcept {
  const double d = a - b;
  return std::isnan(d) || d == std::numeric_limits<double>::infinity() ? kNotMeasuredD : d;
}

inline float power_to_offset_db(double power, double offset_db, double floor_power,
                                float floor_db) noexcept {
  return power < floor_power ? floor_db : static_cast<float>(offset_db + power_to_db_scalar(power));
}

}  // namespace sonare
