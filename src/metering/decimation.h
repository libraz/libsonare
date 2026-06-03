#pragma once

/// @file decimation.h
/// @brief Shared mid/side computation and point-cloud decimation for the stereo
///        scopes (vectorscope, phase scope).
/// @details Both the vectorscope and the phase scope derive mid/side from a
///          left/right sample pair and, when a point budget is requested,
///          down-sample the per-sample point cloud into a fixed number of
///          contiguous buckets keeping the largest-radius sample of each bucket
///          so transient stereo peaks survive. That logic used to be duplicated
///          (and could silently diverge) between metering/stereo.cpp and
///          metering/phase_scope.cpp; it lives here so there is a single source
///          of truth for both the mid/side scaling and the bucket boundaries.

#include <cstddef>

#include "util/constants.h"

namespace sonare::metering::detail {

struct MidSide {
  float mid = 0.0f;
  float side = 0.0f;
};

/// @brief Mid/side for sample @p i of separate left/right buffers.
/// @details Uses the 1/sqrt(2) energy-preserving scaling shared by both scopes.
inline MidSide mid_side(const float* left, const float* right, size_t i) {
  MidSide ms;
  ms.mid = (left[i] + right[i]) * sonare::constants::kInvSqrt2;
  ms.side = (left[i] - right[i]) * sonare::constants::kInvSqrt2;
  return ms;
}

/// @brief Half-open sample range [begin, end) covered by decimation bucket @p b.
/// @details Splits [0, length) into @p max_points contiguous buckets using the
///          same integer math both scopes relied on. An empty bucket
///          (begin == end) is possible when max_points > length and must be
///          skipped by the caller.
struct BucketRange {
  size_t begin = 0;
  size_t end = 0;
};

inline BucketRange decimation_bucket(size_t bucket, size_t length, size_t max_points) {
  BucketRange range;
  range.begin = (bucket * length) / max_points;
  range.end = ((bucket + 1) * length) / max_points;
  return range;
}

/// @brief Decimate [0, length) into at most @p max_points points.
/// @details For each contiguous bucket, builds points via @p make (i -> Point)
///          and keeps the one maximizing @p metric (Point -> float), so the
///          largest-radius sample per bucket survives. Each kept point is handed
///          to @p emit. This is the exact behavior both scopes implemented
///          inline; the per-scope difference is only the @p make / @p metric
///          functors, which are passed in so the numeric results are unchanged.
template <typename MakePoint, typename Metric, typename Emit>
void decimate_max(size_t length, size_t max_points, MakePoint make, Metric metric, Emit emit) {
  for (size_t b = 0; b < max_points; ++b) {
    const BucketRange range = decimation_bucket(b, length, max_points);
    if (range.begin >= range.end) continue;
    auto best = make(range.begin);
    float best_metric = metric(best);
    for (size_t i = range.begin + 1; i < range.end; ++i) {
      auto candidate = make(i);
      const float candidate_metric = metric(candidate);
      if (candidate_metric > best_metric) {
        best_metric = candidate_metric;
        best = candidate;
      }
    }
    emit(best);
  }
}

}  // namespace sonare::metering::detail
