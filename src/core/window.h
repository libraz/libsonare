#pragma once

/// @file window.h
/// @brief Window function generators.

#include <vector>

#include "util/types.h"

namespace sonare {

/// @brief Creates a window of the specified type.
/// @param type Window type
/// @param length Window length in samples
/// @return Vector containing window coefficients
std::vector<float> create_window(WindowType type, int length);

/// @brief Returns a cached window of the specified type (thread-local cache).
/// @param type Window type
/// @param length Window length in samples
/// @return Const reference to cached window coefficients
/// @details Returns a reference to a cached window. More efficient for repeated calls
///          with the same parameters (common in STFT/iSTFT). Thread-local for safety.
const std::vector<float>& get_window_cached(WindowType type, int length);

/// @brief Creates a Hann (raised cosine) window.
/// @param length Window length in samples
/// @return Vector containing window coefficients
std::vector<float> hann_window(int length);

/// @brief Creates a Hamming window.
/// @param length Window length in samples
/// @return Vector containing window coefficients
std::vector<float> hamming_window(int length);

/// @brief Creates a Blackman window.
/// @param length Window length in samples
/// @return Vector containing window coefficients
std::vector<float> blackman_window(int length);

/// @brief Creates a rectangular (boxcar) window.
/// @param length Window length in samples
/// @return Vector containing all 1.0 values
std::vector<float> rectangular_window(int length);

}  // namespace sonare
