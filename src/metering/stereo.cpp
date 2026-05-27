#include "metering/stereo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::metering {

using sonare::constants::kInvSqrt2;

namespace {

void validate_stereo_buffers(const float* left, const float* right, size_t length) {
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(left != nullptr && right != nullptr, ErrorCode::InvalidParameter);
}

}  // namespace

float correlation(const float* left, const float* right, size_t length) {
  validate_stereo_buffers(left, right, length);

  double dot = 0.0;
  double left_energy = 0.0;
  double right_energy = 0.0;
  for (size_t i = 0; i < length; ++i) {
    dot += static_cast<double>(left[i]) * static_cast<double>(right[i]);
    left_energy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
    right_energy += static_cast<double>(right[i]) * static_cast<double>(right[i]);
  }

  const double denom = std::sqrt(left_energy * right_energy);
  if (denom < kEpsilon) return 0.0f;

  const double value = dot / denom;
  return static_cast<float>(std::clamp(value, -1.0, 1.0));
}

float stereo_width(const float* left, const float* right, size_t length) {
  validate_stereo_buffers(left, right, length);

  double mid_energy = 0.0;
  double side_energy = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const float mid = (left[i] + right[i]) * kInvSqrt2;
    const float side = (left[i] - right[i]) * kInvSqrt2;
    mid_energy += static_cast<double>(mid) * static_cast<double>(mid);
    side_energy += static_cast<double>(side) * static_cast<double>(side);
  }

  if (mid_energy < kEpsilon) {
    return side_energy < kEpsilon ? 0.0f : std::numeric_limits<float>::infinity();
  }

  return static_cast<float>(std::sqrt(side_energy / mid_energy));
}

std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length) {
  validate_stereo_buffers(left, right, length);

  std::vector<VectorscopePoint> points(length);
  for (size_t i = 0; i < length; ++i) {
    points[i].mid = (left[i] + right[i]) * kInvSqrt2;
    points[i].side = (left[i] - right[i]) * kInvSqrt2;
  }
  return points;
}

}  // namespace sonare::metering
