#include "metering/phase_scope.h"

#include <algorithm>
#include <cmath>

#include "metering/decimation.h"
#include "metering/stereo.h"
#include "util/exception.h"

namespace sonare::metering {

namespace {

void validate_stereo_buffers(const float* left, const float* right, size_t length) {
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(left != nullptr && right != nullptr, ErrorCode::InvalidParameter);
}

}  // namespace

namespace {

PhaseScopePoint make_point(const float* left, const float* right, size_t i) {
  const detail::MidSide ms = detail::mid_side(left, right, i);
  PhaseScopePoint point;
  point.mid = ms.mid;
  point.side = ms.side;
  point.radius = std::sqrt(point.mid * point.mid + point.side * point.side);
  point.angle_rad = std::atan2(point.side, point.mid);
  return point;
}

}  // namespace

PhaseScopeResult phase_scope(const float* left, const float* right, size_t length) {
  return phase_scope(left, right, length, 0);
}

PhaseScopeResult phase_scope(const float* left, const float* right, size_t length,
                             size_t max_points) {
  validate_stereo_buffers(left, right, length);

  PhaseScopeResult result;
  result.correlation = correlation(left, right, length);

  // Summary stats are computed over the full-resolution signal regardless of the
  // requested point budget, so decimation only affects the returned point cloud.
  double abs_angle_sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const PhaseScopePoint point = make_point(left, right, i);
    abs_angle_sum += std::abs(point.angle_rad);
    result.max_radius = std::max(result.max_radius, point.radius);
  }
  result.average_abs_angle_rad = static_cast<float>(abs_angle_sum / static_cast<double>(length));

  if (max_points == 0 || length <= max_points) {
    result.points.resize(length);
    for (size_t i = 0; i < length; ++i) result.points[i] = make_point(left, right, i);
    return result;
  }

  // Deterministic decimation into max_points contiguous buckets, keeping the
  // largest-radius sample of each bucket so transient peaks are preserved. Shared
  // bucket math lives in detail::decimate_max so it cannot diverge from the
  // vectorscope.
  result.points.reserve(max_points);
  detail::decimate_max(
      length, max_points, [&](size_t i) { return make_point(left, right, i); },
      [](const PhaseScopePoint& p) { return p.radius; },
      [&](const PhaseScopePoint& best) { result.points.push_back(best); });
  return result;
}

}  // namespace sonare::metering
