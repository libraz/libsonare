#include "analysis/meter/phase_scope.h"

#include <algorithm>
#include <cmath>

#include "analysis/meter/stereo.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::analysis::meter {

using sonare::constants::kInvSqrt2;

namespace {

void validate_stereo_buffers(const float* left, const float* right, size_t length) {
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(left != nullptr && right != nullptr, ErrorCode::InvalidParameter);
}

}  // namespace

PhaseScopeResult phase_scope(const float* left, const float* right, size_t length) {
  validate_stereo_buffers(left, right, length);

  PhaseScopeResult result;
  result.points.resize(length);
  result.correlation = correlation(left, right, length);

  double abs_angle_sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    PhaseScopePoint& point = result.points[i];
    point.mid = (left[i] + right[i]) * kInvSqrt2;
    point.side = (left[i] - right[i]) * kInvSqrt2;
    point.radius = std::sqrt(point.mid * point.mid + point.side * point.side);
    point.angle_rad = std::atan2(point.side, point.mid);
    abs_angle_sum += std::abs(point.angle_rad);
    result.max_radius = std::max(result.max_radius, point.radius);
  }

  result.average_abs_angle_rad = static_cast<float>(abs_angle_sum / static_cast<double>(length));
  return result;
}

}  // namespace sonare::analysis::meter
