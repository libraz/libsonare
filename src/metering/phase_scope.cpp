#include "metering/phase_scope.h"

#include <algorithm>
#include <cmath>

#include "metering/decimation.h"
#include "metering/stereo.h"
#include "util/db.h"
#include "util/parameter_checks.h"

namespace sonare::metering {

namespace {

void validate_stereo_buffers(const float* left, const float* right, size_t length) {
  if (length == 0) {
    util::check::refuse("length", util::refusal::integer_at_least_predicate(1));
  }
  util::check::non_null(left, "left");
  util::check::non_null(right, "right");
}

}  // namespace

namespace {

PhaseScopePoint make_point(const float* left, const float* right, size_t i) {
  const detail::MidSide ms = detail::mid_side(left, right, i);
  PhaseScopePoint point;
  point.mid = ms.mid;
  point.side = ms.side;
  point.radius = std::sqrt(point.mid * point.mid + point.side * point.side);
  point.angle_rad = point.radius == 0.0f ? kNotMeasured : std::atan2(point.side, point.mid);
  return point;
}

// A silent sample counts as angle 0 in the mean, as it always has, so a mean over audio that
// holds exact zeros is unchanged.
double abs_angle_or_zero(const PhaseScopePoint& point) {
  return std::isnan(point.angle_rad) ? 0.0 : std::abs(point.angle_rad);
}

// All-silent input has no angle at all.
float average_abs_angle(double abs_angle_sum, size_t length, float max_radius) {
  if (max_radius == 0.0f) return kNotMeasured;
  return static_cast<float>(abs_angle_sum / static_cast<double>(length));
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

  if (max_points == 0 || length <= max_points) {
    // Full-resolution path: fill the point cloud and accumulate the summary
    // stats in a single pass so atan2/sqrt are evaluated once per sample.
    result.points.resize(length);
    for (size_t i = 0; i < length; ++i) {
      const PhaseScopePoint point = make_point(left, right, i);
      result.points[i] = point;
      abs_angle_sum += abs_angle_or_zero(point);
      result.max_radius = std::max(result.max_radius, point.radius);
    }
    result.average_abs_angle_rad = average_abs_angle(abs_angle_sum, length, result.max_radius);
    return result;
  }

  // Deterministic decimation into max_points contiguous buckets, keeping the
  // largest-radius sample of each bucket so transient peaks are preserved. Shared
  // bucket math lives in detail::decimate_max so it cannot diverge from the
  // vectorscope.
  result.points.reserve(max_points);
  // decimate_max visits every index once, ascending, so the summary accumulates in make.
  detail::decimate_max(
      length, max_points,
      [&](size_t i) {
        const PhaseScopePoint point = make_point(left, right, i);
        abs_angle_sum += abs_angle_or_zero(point);
        result.max_radius = std::max(result.max_radius, point.radius);
        return point;
      },
      [](const PhaseScopePoint& p) { return p.radius; },
      [&](const PhaseScopePoint& best) { result.points.push_back(best); });
  result.average_abs_angle_rad = average_abs_angle(abs_angle_sum, length, result.max_radius);
  return result;
}

}  // namespace sonare::metering
