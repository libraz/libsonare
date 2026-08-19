/// @file remix.cpp
/// @brief Implementation of time-domain remixing.

#include "effects/remix.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Returns the indices where `y` changes sign, with librosa-compatible
///        zero handling.
/// @details Equivalent to `sonare::zero_crossings(y, n, threshold,
///          ref_magnitude=false, pad=true, zero_pos=true)` from
///          `feature/spectral.h`, inlined here so the effects layer does not
///          depend on `feature/`. Mirrors `librosa.zero_crossings` semantics:
///          values with |v| <= threshold are treated as zero, and the sign of
///          zero is considered positive (uses `std::signbit`). With pad=true,
///          index 0 is always reported.
std::vector<int> zero_crossings_for_remix(const float* y, std::size_t n, float threshold) {
  std::vector<int> indices;
  if (n == 0) return indices;

  auto sample_sign = [&](float v) -> int {
    if (v >= -threshold && v <= threshold) v = 0.0f;
    return std::signbit(v) ? -1 : +1;
  };

  // pad=true: index 0 is always reported.
  indices.push_back(0);

  int prev_sign = sample_sign(y[0]);
  for (std::size_t i = 1; i < n; ++i) {
    const int cur_sign = sample_sign(y[i]);
    if (cur_sign != prev_sign) {
      indices.push_back(static_cast<int>(i));
    }
    prev_sign = cur_sign;
  }
  return indices;
}

/// @brief Returns the element of `sorted_zeros` closest to `value`.
/// @details Mirrors librosa.util.match_events (with left=right=True).
int match_event(int value, const std::vector<int>& sorted_zeros) {
  if (sorted_zeros.empty()) return value;
  // Lower bound on sorted_zeros (first element >= value).
  auto it = std::lower_bound(sorted_zeros.begin(), sorted_zeros.end(), value);
  if (it == sorted_zeros.end()) {
    return sorted_zeros.back();
  }
  if (it == sorted_zeros.begin()) {
    return *it;
  }
  int right_val = *it;
  int left_val = *(it - 1);
  // Prefer the closer one (ties -> right, matching argmin behaviour on equal
  // distance which returns the first occurrence; numpy argmin returns the
  // earliest minimum, so prefer left on tie).
  int dl = std::abs(value - left_val);
  int dr = std::abs(right_val - value);
  return (dl <= dr) ? left_val : right_val;
}

}  // namespace

std::vector<std::pair<int, int>> align_remix_intervals(
    const float* y, std::size_t n, const std::vector<std::pair<int, int>>& intervals,
    bool align_zeros) {
  if (n > 0 && y == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "align_remix_intervals: null input with non-zero length");
  }
  const int length = static_cast<int>(n);
  const auto clamp_pair = [length](int start, int end) {
    if (start < 0) start = 0;
    if (start > length) start = length;
    if (end > length) end = length;
    if (end < start) end = start;
    return std::pair<int, int>{start, end};
  };

  std::vector<int> zeros;
  bool snap = false;
  if (align_zeros) {
    zeros = zero_crossings_for_remix(y, n, constants::kEpsilon);
    // zero_crossings_for_remix always reports index 0 (pad=true), so a signal
    // with no real sign change yields exactly {0} and, once the end sentinel is
    // appended, the snap set {0, n}. Snapping to that drags every boundary onto
    // 0 or n and erases the slice — which is what silence, a DC offset and any
    // constant all do. Treat "no crossing found" as "nothing to snap to".
    snap = zeros.size() > 1;
    zeros.push_back(length);
  }

  std::vector<std::pair<int, int>> out;
  out.reserve(intervals.size());
  for (const auto& iv : intervals) {
    const std::pair<int, int> raw = clamp_pair(iv.first, iv.second);
    if (!snap) {
      out.push_back(raw);
      continue;
    }
    const std::pair<int, int> snapped =
        clamp_pair(match_event(iv.first, zeros), match_event(iv.second, zeros));
    // A slice that had content before snapping must keep some: with sparse
    // crossings both boundaries can land on the same point.
    out.push_back(snapped.second > snapped.first || raw.second <= raw.first ? snapped : raw);
  }
  return out;
}

std::vector<std::pair<int, int>> align_remix_intervals(
    const std::vector<float>& y, const std::vector<std::pair<int, int>>& intervals,
    bool align_zeros) {
  return align_remix_intervals(y.data(), y.size(), intervals, align_zeros);
}

std::vector<float> remix(const float* y, std::size_t n,
                         const std::vector<std::pair<int, int>>& intervals, bool align_zeros) {
  if (n > 0 && y == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "remix: null input with non-zero length");
  }
  const std::vector<std::pair<int, int>> resolved =
      align_remix_intervals(y, n, intervals, align_zeros);

  std::vector<float> out;
  for (const auto& iv : resolved) {
    if (iv.second <= iv.first) continue;
    out.insert(out.end(), y + iv.first, y + iv.second);
  }
  return out;
}

std::vector<float> remix(const std::vector<float>& y,
                         const std::vector<std::pair<int, int>>& intervals, bool align_zeros) {
  return remix(y.data(), y.size(), intervals, align_zeros);
}

}  // namespace sonare
