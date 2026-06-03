#pragma once

/// @file phase_scope.h
/// @brief Phase scope data for stereo signals.

#include <cstddef>
#include <vector>

namespace sonare::metering {

struct PhaseScopePoint {
  float mid = 0.0f;
  float side = 0.0f;
  float radius = 0.0f;
  float angle_rad = 0.0f;
};

struct PhaseScopeResult {
  std::vector<PhaseScopePoint> points;
  float correlation = 0.0f;
  float average_abs_angle_rad = 0.0f;
  float max_radius = 0.0f;
};

/// @brief Phase scope with one point per input sample.
PhaseScopeResult phase_scope(const float* left, const float* right, size_t length);

/// @brief Display-sized phase scope.
/// @param max_points Upper bound on the number of returned @c points. When 0, or
///        when @p length <= @p max_points, every sample is emitted. Otherwise the
///        point series is deterministically decimated to at most @p max_points
///        points (keeping the largest-radius sample per stride so transients
///        survive). The summary stats (@c correlation, @c average_abs_angle_rad,
///        @c max_radius) are always computed over the full-resolution signal, so
///        they are unaffected by decimation.
PhaseScopeResult phase_scope(const float* left, const float* right, size_t length,
                             size_t max_points);

}  // namespace sonare::metering
