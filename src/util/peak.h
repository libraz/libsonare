#pragma once

/// @file peak.h
/// @brief Peak picking utility (librosa.util.peak_pick compatible).

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Pick peaks in a 1-D signal using local maxima with hysteresis.
/// @param x Input array
/// @param n Length of x
/// @param pre_max Window radius (samples before) for local-max comparison
/// @param post_max Window radius (samples after) for local-max comparison
/// @param pre_avg Window radius (samples before) for moving-average baseline
/// @param post_avg Window radius (samples after) for moving-average baseline
/// @param delta Threshold above moving-average baseline
/// @param wait Minimum number of samples between consecutive peaks
/// @return Sorted vector of peak indices.
/// @details Same algorithm as librosa.util.peak_pick: a sample `i` is a peak
///          when `x[i] == max(x[i-pre_max : i+post_max])` and
///          `x[i] >= mean(x[i-pre_avg : i+post_avg]) + delta`, greedily spaced by
///          `wait`. The window high ends (`i+post_max` / `i+post_avg`) are
///          EXCLUSIVE, matching librosa's array slices.
/// @throw sonare::SonareException if any radius or wait is negative.
std::vector<int> peak_pick(const float* x, std::size_t n, int pre_max, int post_max, int pre_avg,
                           int post_avg, float delta, int wait);
std::vector<int> peak_pick(const std::vector<float>& x, int pre_max, int post_max, int pre_avg,
                           int post_avg, float delta, int wait);

/// @brief Sliding-window maximum with an inclusive symmetric window.
/// @param x Input array (finite values)
/// @param n Length of x
/// @param radius Window radius in samples
/// @return `y[i] = max(x[max(0, i - radius) .. min(n - 1, i + radius)])`; empty if n == 0.
/// @details O(n) monotonic deque. `radius >= n` yields the global maximum everywhere.
std::vector<float> sliding_max(const float* x, std::size_t n, std::size_t radius);

/// @brief Select the strongest candidates that are at least `min_distance` apart.
/// @param candidates Candidate frame indices (valid indices into `values`)
/// @param values Strength per frame
/// @param min_distance Minimum spacing between accepted frames
/// @return Accepted frames in ascending order.
/// @details Candidates are visited by descending `values[frame]` (ties: earlier frame first) and
///          accepted when `|i - j| >= min_distance` for every accepted `j`, like scipy
///          `find_peaks(distance=)`. `min_distance <= 1` returns every candidate.
std::vector<int> select_peaks_min_distance(const std::vector<int>& candidates, const float* values,
                                           int min_distance);

}  // namespace sonare
