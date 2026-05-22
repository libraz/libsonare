#pragma once

/// @file vector_normalize.h
/// @brief Vector / matrix normalization (librosa.util.normalize compatible).
/// @details Distinct from sonare::normalize(const Audio&, float) in
///          src/effects/normalize.h, which performs peak / RMS gain on Audio.

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Normalization mode.
enum class NormType {
  Inf,    ///< max(|x|)
  L1,     ///< sum(|x|)
  L2,     ///< sqrt(sum(x^2))
  Power,  ///< sum(x^2)
};

/// @brief Normalize a 1-D array in place into a new vector.
/// @param x Input data
/// @param n Length of x
/// @param norm Norm to apply
/// @param threshold Below this norm value the input is returned unchanged
///        (clamped to a copy). Use 0 to always normalize when possible.
/// @return Normalized data of length n.
/// @details If the computed norm is < max(threshold, tiny_eps), the result is
///          a copy of the input (matches librosa's `fill=None` behavior).
/// @throw std::invalid_argument if n > 0 and x is null.
std::vector<float> normalize(const float* x, std::size_t n, NormType norm = NormType::Inf,
                             float threshold = 0.0f);
std::vector<float> normalize(const std::vector<float>& x, NormType norm = NormType::Inf,
                             float threshold = 0.0f);

/// @brief Normalize each row or column of a row-major matrix.
/// @param x Row-major matrix data, length rows * cols
/// @param rows Number of rows
/// @param cols Number of columns
/// @param axis 0 to normalize along columns (each column treated as a vector),
///             1 to normalize along rows (each row treated as a vector).
///             Matches librosa: axis=0 normalizes over rows-per-column,
///             axis=1 normalizes over cols-per-row.
/// @param norm Norm to apply
/// @param threshold Below this norm the segment is left unchanged
/// @return Normalized matrix data (same shape as input).
std::vector<float> normalize_matrix(const float* x, int rows, int cols, int axis,
                                    NormType norm = NormType::Inf, float threshold = 0.0f);

}  // namespace sonare
