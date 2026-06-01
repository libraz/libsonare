#pragma once

/// @file frame.h
/// @brief Sliding-window framing utility (librosa.util.frame compatible).

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Slice signal into overlapping frames.
/// @param x Input signal
/// @param n Length of x
/// @param frame_length Length of each frame
/// @param hop_length Hop between frames (>0)
/// @return Flat array of size [n_frames * frame_length] in row-major order.
///         Frame i occupies indices [i * frame_length, (i+1) * frame_length).
/// @details Number of frames is floor((n - frame_length) / hop_length) + 1.
///          If n < frame_length, returns an empty vector.
///          Mirrors librosa.util.frame with axis=-1 (frames stored along the
///          last dimension of the input, the trailing axis in the output).
/// @throw sonare::SonareException if frame_length <= 0 or hop_length <= 0,
///        or if x is null when n > 0.
std::vector<float> frame(const float* x, std::size_t n, int frame_length, int hop_length);

/// @brief std::vector overload.
std::vector<float> frame(const std::vector<float>& x, int frame_length, int hop_length);

/// @brief Compute the number of frames that frame() would produce.
/// @return floor((n - frame_length) / hop_length) + 1 when n >= frame_length, else 0.
int frame_count(std::size_t n, int frame_length, int hop_length);

}  // namespace sonare
