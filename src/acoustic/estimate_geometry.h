#pragma once

/// @file estimate_geometry.h
/// @brief The two measurability rules that decide what of a room estimate can seed a room for
///        RIR synthesis, shared by every surface that maps an estimate onto synthesizer input.

#include <cmath>
#include <cstddef>

namespace sonare::acoustic {

/// @brief True when all three dimensions are finite and positive, i.e. a decay was measurable.
inline bool estimated_dimensions_measured(float length, float width, float height) {
  const auto usable = [](float dimension) { return std::isfinite(dimension) && dimension > 0.0f; };
  return usable(length) && usable(width) && usable(height);
}

/// @brief True when there is at least one band and every one is finite.
/// @details A band that did not converge is NaN-filled in an estimate; a set containing one is
///          left out of the geometry, so the synthesizer's scalar absorption applies.
inline bool estimated_absorption_measured(const float* bands, std::size_t count) {
  if (bands == nullptr || count == 0) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(bands[i])) return false;
  }
  return true;
}

}  // namespace sonare::acoustic
