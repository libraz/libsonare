#pragma once

/// @file dct.h
/// @brief Discrete Cosine Transform (DCT-II) implementation.

#include <vector>

namespace sonare {

/// @brief Creates DCT-II matrix with orthonormal normalization.
/// @param n_output Number of output coefficients (rows)
/// @param n_input Number of input values (columns)
/// @return DCT matrix [n_output x n_input] in row-major order
/// @details Implements DCT-II with orthonormal normalization (scipy 'ortho' mode).
std::vector<float> create_dct_matrix(int n_output, int n_input);

/// @brief Applies DCT-II to input vector.
/// @param input Input values
/// @param n_input Number of input values
/// @param n_output Number of output coefficients (0 = same as input)
/// @return DCT coefficients
std::vector<float> dct_ii(const float* input, int n_input, int n_output = 0);

/// @brief Applies DCT-II to input vector.
/// @param input Input values
/// @param n_output Number of output coefficients (0 = same as input)
/// @return DCT coefficients
std::vector<float> dct_ii(const std::vector<float>& input, int n_output = 0);

/// @brief Applies inverse DCT-II (DCT-III with scaling).
/// @param input DCT coefficients
/// @param n_input Number of input coefficients
/// @param n_output Number of output values (0 = same as input)
/// @return Reconstructed values
std::vector<float> idct_ii(const float* input, int n_input, int n_output = 0);

}  // namespace sonare
