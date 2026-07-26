/// @file peak.cpp
/// @brief Implementation of peak picking.

#include "util/peak.h"

#include <algorithm>

#include "util/exception.h"

namespace sonare {

std::vector<int> peak_pick(const float* x, std::size_t n, int pre_max, int post_max, int pre_avg,
                           int post_avg, float delta, int wait) {
  if (pre_max < 0 || post_max <= 0 || pre_avg < 0 || post_avg <= 0 || wait < 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "peak_pick: post windows must be positive and other windows non-negative");
  }
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "peak_pick: null input with non-zero length");
  }
  std::vector<int> peaks;
  if (n == 0) return peaks;

  const int N = static_cast<int>(n);
  int last_peak = -wait - 1;  // ensures first candidate is allowed
  for (int i = 0; i < N; ++i) {
    // Local max window, matching librosa's x[i - pre_max : i + post_max] slice:
    // the high end i + post_max is EXCLUSIVE, so the inclusive upper index is
    // i + post_max - 1 (an inclusive i + post_max would scan one sample too far
    // and pass spurious peaks on a plateau's trailing edge).
    const int max_lo = std::max(0, i - pre_max);
    const int max_hi = std::min(N - 1, i + post_max - 1);
    bool is_max = true;
    for (int k = max_lo; k <= max_hi; ++k) {
      if (x[k] > x[i]) {
        is_max = false;
        break;
      }
    }
    if (!is_max) continue;

    // Moving average baseline over librosa's x[i - pre_avg : i + post_avg] slice
    // (high end exclusive -> inclusive i + post_avg - 1).
    const int avg_lo = std::max(0, i - pre_avg);
    const int avg_hi = std::min(N - 1, i + post_avg - 1);
    double sum = 0.0;
    for (int k = avg_lo; k <= avg_hi; ++k) sum += x[k];
    const int avg_count = avg_hi - avg_lo + 1;
    const float avg = static_cast<float>(sum / static_cast<double>(avg_count));

    if (x[i] < avg + delta) continue;
    if (i - last_peak <= wait) continue;
    peaks.push_back(i);
    last_peak = i;
  }
  return peaks;
}

std::vector<int> peak_pick(const std::vector<float>& x, int pre_max, int post_max, int pre_avg,
                           int post_avg, float delta, int wait) {
  return peak_pick(x.data(), x.size(), pre_max, post_max, pre_avg, post_avg, delta, wait);
}

}  // namespace sonare
