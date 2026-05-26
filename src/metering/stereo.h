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
std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length);

}  // namespace sonare::metering
