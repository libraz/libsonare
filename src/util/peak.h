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

}  // namespace sonare
