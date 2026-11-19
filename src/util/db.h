#pragma once

/// @file db.h
/// @brief Scalar decibel conversion helpers.

#include <cmath>

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

inline float power_to_offset_db(double power, double offset_db, double floor_power,
                                float floor_db) noexcept {
  return power < floor_power ? floor_db : static_cast<float>(offset_db + power_to_db_scalar(power));
}

}  // namespace sonare
