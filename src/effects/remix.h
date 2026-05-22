#pragma once

/// @file remix.h
/// @brief Time-domain remixing of audio segments
///        (librosa.effects.remix compatible).

#include <cstddef>
#include <utility>
#include <vector>

namespace sonare {

/// @brief Reorders / concatenates a signal by interval slices.
/// @details Each interval `(start, end)` selects samples `y[start..end)`
///          (end-exclusive). The output is the concatenation of all such
///          slices in order. When `align_zeros` is true, both `start` and
///          `end` are snapped to the nearest zero-crossing of `y`
///          (matching `librosa.effects.remix`).
/// @param y Input signal.
/// @param n Number of samples.
/// @param intervals Sequence of (start, end) sample boundaries.
/// @param align_zeros Snap to zero-crossings (default true).
/// @return Remixed signal.
/// @throw std::invalid_argument on null input with n > 0 or invalid intervals.
std::vector<float> remix(const float* y, std::size_t n,
                         const std::vector<std::pair<int, int>>& intervals,
                         bool align_zeros = true);
std::vector<float> remix(const std::vector<float>& y,
                         const std::vector<std::pair<int, int>>& intervals,
                         bool align_zeros = true);

}  // namespace sonare
