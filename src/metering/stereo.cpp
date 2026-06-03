#include "metering/stereo.h"

#include "util/constants.h"
#include "util/exception.h"
#include "util/stereo_metrics.h"

namespace sonare::metering {

using sonare::constants::kInvSqrt2;

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
    VectorscopePoint p;
    p.mid = (left[i] + right[i]) * kInvSqrt2;
    p.side = (left[i] - right[i]) * kInvSqrt2;
    return p;
  };

  if (max_points == 0 || length <= max_points) {
    std::vector<VectorscopePoint> points(length);
    for (size_t i = 0; i < length; ++i) points[i] = point_at(i);
    return points;
  }

  // Deterministic decimation: split the input into max_points contiguous buckets
  // and keep, for each bucket, the sample with the largest radius (|mid|+|side|
  // proxy) so transient stereo peaks survive the down-sample.
  std::vector<VectorscopePoint> points;
  points.reserve(max_points);
  for (size_t b = 0; b < max_points; ++b) {
    const size_t begin = (b * length) / max_points;
    const size_t end = ((b + 1) * length) / max_points;
    if (begin >= end) continue;
    VectorscopePoint best = point_at(begin);
    float best_metric = best.mid * best.mid + best.side * best.side;
    for (size_t i = begin + 1; i < end; ++i) {
      const VectorscopePoint p = point_at(i);
      const float metric = p.mid * p.mid + p.side * p.side;
      if (metric > best_metric) {
        best_metric = metric;
        best = p;
      }
    }
    points.push_back(best);
  }
  return points;
}

}  // namespace sonare::metering
