#pragma once

/// @file estimate_geometry.h
/// @brief The two measurability rules that decide what of a room estimate can seed a room for
///        RIR synthesis, shared by every surface that maps an estimate onto synthesizer input.

#include <cmath>
#include <cstddef>
#include <limits>

#include "acoustic/room_model.h"

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

/// @brief Mean of the finite bands; NaN when there is none.
inline float mean_finite_absorption(const float* bands, std::size_t count) {
  double sum = 0.0;
  std::size_t finite = 0;
  for (std::size_t i = 0; bands != nullptr && i < count; ++i) {
    if (!std::isfinite(bands[i])) continue;
    sum += bands[i];
    ++finite;
  }
  if (finite == 0) return std::numeric_limits<float>::quiet_NaN();
  return static_cast<float>(sum / static_cast<double>(finite));
}

/// @brief The default source and listener positions scaled per axis from the default room to a
///        room of the given dimensions, so they stay inside it; the default room maps to
///        `kDefaultPlacement` exactly.
inline SourceListener scaled_default_placement(float length, float width, float height) {
  const float sx = length / kDefaultRoomDims.length;
  const float sy = width / kDefaultRoomDims.width;
  const float sz = height / kDefaultRoomDims.height;
  const auto scale = [&](const Vec3& p) { return Vec3{p.x * sx, p.y * sy, p.z * sz}; };
  return {scale(kDefaultPlacement.source), scale(kDefaultPlacement.listener)};
}

}  // namespace sonare::acoustic
