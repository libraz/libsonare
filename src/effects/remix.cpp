/// @file remix.cpp
/// @brief Implementation of time-domain remixing.

#include "effects/remix.h"

#include <algorithm>
#include <stdexcept>

#include "feature/spectral.h"

namespace sonare {

namespace {

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

std::vector<float> remix(const float* y, std::size_t n,
                         const std::vector<std::pair<int, int>>& intervals, bool align_zeros) {
  if (n > 0 && y == nullptr) {
    throw std::invalid_argument("remix: null input with non-zero length");
  }

  std::vector<int> zeros;
  if (align_zeros) {
    zeros = zero_crossings(y, n, 1e-10f, false, true, true);
    // Force end-of-signal onto zeros (mirrors librosa).
    zeros.push_back(static_cast<int>(n));
  }

  std::vector<float> out;
  for (const auto& iv : intervals) {
    int start = iv.first;
    int end = iv.second;
    if (align_zeros) {
      start = match_event(start, zeros);
      end = match_event(end, zeros);
    }
    if (start < 0) start = 0;
    if (end > static_cast<int>(n)) end = static_cast<int>(n);
    if (end <= start) continue;
    out.insert(out.end(), y + start, y + end);
  }
  return out;
}

std::vector<float> remix(const std::vector<float>& y,
                         const std::vector<std::pair<int, int>>& intervals, bool align_zeros) {
  return remix(y.data(), y.size(), intervals, align_zeros);
}

}  // namespace sonare
