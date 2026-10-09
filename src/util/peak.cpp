/// @file peak.cpp
/// @brief Implementation of peak picking.

#include "util/peak.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

std::vector<float> sliding_max(const float* x, std::size_t n, std::size_t radius) {
  std::vector<float> y(n);
  if (n == 0) return y;
  if (x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sliding_max: null input with non-zero length");
  }
  // Indices whose values are decreasing, from window[head]; the head is the window maximum.
  // Each index is pushed once, so a vector with a moving head is the whole queue.
  std::vector<std::size_t> window;
  window.reserve(n);
  std::size_t head = 0;
  std::size_t next = 0;  // next index to push
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t hi = (radius >= n - 1 - i) ? n - 1 : i + radius;
    for (; next <= hi; ++next) {
      while (window.size() > head && x[window.back()] <= x[next]) window.pop_back();
      window.push_back(next);
    }
    const std::size_t lo = (i > radius) ? i - radius : 0;
    while (window[head] < lo) ++head;
    y[i] = x[window[head]];
  }
  return y;
}

std::vector<float> sliding_max_without_lone_peaks(const float* x, std::size_t n, std::size_t radius,
                                                  const std::vector<int>& events, float ratio) {
  std::vector<float> y = sliding_max(x, n, radius);
  std::vector<float> heights;
  std::size_t prev_lo = 0;
  std::size_t prev_hi = 0;
  float prev_ref = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const auto lo_value = static_cast<long long>(i) - static_cast<long long>(radius);
    const auto hi_value =
        (radius >= n - 1 - i) ? static_cast<long long>(n - 1) : static_cast<long long>(i + radius);
    const auto lo = std::lower_bound(events.begin(), events.end(), lo_value,
                                     [](int e, long long v) { return e < v; });
    const auto hi = std::upper_bound(events.begin(), events.end(), hi_value,
                                     [](long long v, int e) { return v < e; });
    if (lo == hi) continue;
    const auto lo_index = static_cast<std::size_t>(lo - events.begin());
    const auto hi_index = static_cast<std::size_t>(hi - events.begin());
    // The event set only changes when the window crosses an event.
    if (lo_index != prev_lo || hi_index != prev_hi || heights.empty()) {
      heights.clear();
      for (auto it = lo; it != hi; ++it) heights.push_back(x[*it]);
      // Ascending reuses the shared float sort; walk down from the largest.
      std::sort(heights.begin(), heights.end());
      std::size_t k = heights.size() - 1;
      while (k > 0 && heights[k - 1] < ratio * heights[k]) --k;
      prev_lo = lo_index;
      prev_hi = hi_index;
      prev_ref = heights[k];
    }
    y[i] = prev_ref;
  }
  return y;
}

std::vector<int> select_peaks_min_distance(const std::vector<int>& candidates, const float* values,
                                           int min_distance) {
  std::vector<int> by_frame(candidates);
  std::sort(by_frame.begin(), by_frame.end());
  if (min_distance <= 1) return by_frame;

  // Stable descending order by value: a candidate's slot is the count of larger values plus
  // the earlier frames sharing its value. Reuses the shared float sort instead of a stable sort.
  const std::size_t n = by_frame.size();
  std::vector<float> sorted_values(n);
  for (std::size_t i = 0; i < n; ++i) {
    sorted_values[i] = values[by_frame[i]];
    // An unordered value would send the slot arithmetic below out of range.
    SONARE_CHECK_MSG(!std::isnan(sorted_values[i]), ErrorCode::InvalidParameter,
                     "select_peaks_min_distance: NaN candidate value");
  }
  std::sort(sorted_values.begin(), sorted_values.end());
  std::vector<std::size_t> filled(n, 0);
  std::vector<int> order(n);
  for (int c : by_frame) {
    const auto not_larger =
        std::upper_bound(sorted_values.begin(), sorted_values.end(), values[c]) -
        sorted_values.begin();
    const std::size_t larger = n - static_cast<std::size_t>(not_larger);
    order[larger + filled[larger]++] = c;
  }
  std::vector<int> accepted;
  for (int c : order) {
    bool ok = true;
    for (int j : accepted) {
      if (std::abs(c - j) < min_distance) {
        ok = false;
        break;
      }
    }
    if (ok) accepted.push_back(c);
  }
  std::sort(accepted.begin(), accepted.end());
  return accepted;
}

float max_excluding_top(std::vector<float> values, std::size_t ignored) {
  if (values.empty()) return 0.0f;
  if (values.size() <= ignored) return *std::max_element(values.begin(), values.end());
  // Ascending reuses the shared nth_element; the ignored largest sit above nth.
  const auto nth = values.end() - 1 - static_cast<std::ptrdiff_t>(ignored);
  std::nth_element(values.begin(), nth, values.end());
  return *nth;
}

}  // namespace sonare
