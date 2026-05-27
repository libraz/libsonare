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

PhaseScopeResult phase_scope(const float* left, const float* right, size_t length);

}  // namespace sonare::metering
