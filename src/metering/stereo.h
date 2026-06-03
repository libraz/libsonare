#pragma once

/// @file stereo.h
/// @brief Stereo image meters for separate left/right sample buffers.

#include <cstddef>
#include <vector>

namespace sonare::metering {

struct VectorscopePoint {
  float mid = 0.0f;
  float side = 0.0f;
};

float correlation(const float* left, const float* right, size_t length);
float stereo_width(const float* left, const float* right, size_t length);

/// @brief Per-sample mid/side vectorscope: one point for every input sample.
std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length);

/// @brief Display-sized mid/side vectorscope.
/// @param max_points Upper bound on the number of returned points. When 0, or
///        when @p length <= @p max_points, every sample is emitted (identical to
///        the per-sample overload). Otherwise the input is deterministically
///        decimated to at most @p max_points points by taking a stride and the
///        per-stride extreme (largest-radius) sample, so transient peaks are
///        preserved instead of averaged away. Decimation is deterministic for a
///        given (length, max_points) pair.
std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length,
                                          size_t max_points);

}  // namespace sonare::metering
