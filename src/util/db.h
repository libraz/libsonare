#pragma once

/// @file db.h
/// @brief Scalar decibel conversion helpers.

#include <cmath>

#include "util/constants.h"

namespace sonare {

inline float db_to_linear(float db) noexcept { return std::pow(10.0f, db / 20.0f); }

inline double db_to_linear(double db) noexcept { return std::pow(10.0, db / 20.0); }

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

}  // namespace sonare
