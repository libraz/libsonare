#include "metering/stereo.h"

#include "metering/decimation.h"
#include "util/exception.h"
#include "util/stereo_metrics.h"

namespace sonare::metering {

float correlation(const float* left, const float* right, size_t length) {
  return util::stereo_correlation(left, right, length);
}

float stereo_width(const float* left, const float* right, size_t length) {
  return util::stereo_width(left, right, length);
}

std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length) {
  return vectorscope(left, right, length, 0);
}

std::vector<VectorscopePoint> vectorscope(const float* left, const float* right, size_t length,
                                          size_t max_points) {
  SONARE_CHECK(length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(left != nullptr && right != nullptr, ErrorCode::InvalidParameter);

  auto point_at = [&](size_t i) {
    const detail::MidSide ms = detail::mid_side(left, right, i);
    VectorscopePoint p;
    p.mid = ms.mid;
    p.side = ms.side;
    return p;
  };

  if (max_points == 0 || length <= max_points) {
    std::vector<VectorscopePoint> points(length);
    for (size_t i = 0; i < length; ++i) points[i] = point_at(i);
    return points;
  }

  // Deterministic decimation: split the input into max_points contiguous buckets
  // and keep, for each bucket, the sample with the largest radius (mid^2 + side^2
  // metric) so transient stereo peaks survive the down-sample. Shared bucket math
  // lives in detail::decimate_max so it cannot diverge from the phase scope.
  std::vector<VectorscopePoint> points;
  points.reserve(max_points);
  detail::decimate_max(
      length, max_points, point_at,
      [](const VectorscopePoint& p) { return p.mid * p.mid + p.side * p.side; },
      [&](const VectorscopePoint& best) { points.push_back(best); });
  return points;
}

}  // namespace sonare::metering
