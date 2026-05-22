#pragma once

/// @file padding.h
/// @brief Padding and length-adjustment utilities (librosa.util compatible).

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Pad a 1-D array by centering it within target size.
/// @param x Input pointer
/// @param n Input length
/// @param size Target size. Must satisfy size >= n.
/// @param pad_value Padding value (default 0)
/// @return Vector of length size with x placed at indices [lpad, lpad + n),
///         where lpad = (size - n) / 2 (matches numpy.pad mode='constant').
/// @throw std::invalid_argument if size < n.
std::vector<float> pad_center(const float* x, std::size_t n, std::size_t size,
                              float pad_value = 0.0f);
std::vector<float> pad_center(const std::vector<float>& x, std::size_t size,
                              float pad_value = 0.0f);

/// @brief Crop or pad to exact length.
/// @details If n > size, returns the first `size` samples. If n < size, pads
///          on the right with pad_value. If n == size, returns a copy.
std::vector<float> fix_length(const float* x, std::size_t n, std::size_t size,
                              float pad_value = 0.0f);
std::vector<float> fix_length(const std::vector<float>& x, std::size_t size,
                              float pad_value = 0.0f);

/// @brief Adjust frame indices to fit within bounds (librosa.util.fix_frames).
/// @param frames Sorted (ascending) frame indices
/// @param x_min Minimum allowed value (inclusive)
/// @param x_max Maximum allowed value (exclusive). If negative, only x_min is enforced.
/// @param pad If true, prepend x_min and append x_max (when set) if missing.
/// @return New vector with bounds applied and duplicates removed.
/// @throw std::invalid_argument if frames is not non-decreasing, or contains
///        a negative value when x_min >= 0.
std::vector<int> fix_frames(const std::vector<int>& frames, int x_min = 0, int x_max = -1,
                            bool pad = true);

}  // namespace sonare
