/// @file vector_normalize.cpp
/// @brief Implementation of vector / matrix normalization.

#include "util/vector_normalize.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare {

namespace {

float compute_norm(const float* x, std::size_t n, NormType type) {
  if (n == 0) return 0.0f;
  switch (type) {
    case NormType::Inf:
      return peak_abs(x, n);
    case NormType::L1: {
      float s = 0.0f;
      for (std::size_t i = 0; i < n; ++i) s += std::abs(x[i]);
      return s;
    }
    case NormType::L2: {
      double s = 0.0;
      for (std::size_t i = 0; i < n; ++i) {
        double v = x[i];
        s += v * v;
      }
      return static_cast<float>(std::sqrt(s));
    }
    case NormType::Power: {
      double s = 0.0;
      for (std::size_t i = 0; i < n; ++i) {
        double v = x[i];
        s += v * v;
      }
      return static_cast<float>(s);
    }
  }
  return 0.0f;
}

/// @brief Norms of columns [c0, c0 + n) of a row-major [rows x cols] matrix.
/// @details Mirrors compute_norm() per column down to its accumulator type. Every column takes
///          its terms in ascending row either way, so the norms match a per-column gather bit
///          for bit while the traversal stays contiguous. `norms` and `acc` are caller-owned so
///          a tile loop reuses them instead of reallocating per tile.
void block_column_norms(const float* x, int rows, int cols, int c0, int n, NormType type,
                        std::vector<float>& norms, std::vector<double>& acc) {
  norms.assign(static_cast<std::size_t>(n), 0.0f);
  if (rows == 0) return;
  const std::size_t stride = static_cast<std::size_t>(cols);
  switch (type) {
    case NormType::Inf:
      for (int r = 0; r < rows; ++r) {
        const float* row = x + static_cast<std::size_t>(r) * stride + c0;
        for (int c = 0; c < n; ++c) norms[c] = std::max(norms[c], std::abs(row[c]));
      }
      return;
    case NormType::L1:
      for (int r = 0; r < rows; ++r) {
        const float* row = x + static_cast<std::size_t>(r) * stride + c0;
        for (int c = 0; c < n; ++c) norms[c] += std::abs(row[c]);
      }
      return;
    case NormType::L2:
    case NormType::Power: {
      acc.assign(static_cast<std::size_t>(n), 0.0);
      for (int r = 0; r < rows; ++r) {
        const float* row = x + static_cast<std::size_t>(r) * stride + c0;
        for (int c = 0; c < n; ++c) {
          const double v = row[c];
          acc[c] += v * v;
        }
      }
      for (int c = 0; c < n; ++c) {
        norms[c] = static_cast<float>(type == NormType::L2 ? std::sqrt(acc[c]) : acc[c]);
      }
      return;
    }
  }
}

}  // namespace

std::vector<float> normalize(const float* x, std::size_t n, NormType norm, float threshold) {
  if (n > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "normalize: null input with non-zero length");
  }
  // A null pointer with zero length is a valid empty transform, but forming
  // the iterator pair [nullptr, nullptr) is not a defined C++ range.
  if (n == 0) return {};
  std::vector<float> out(x, x + n);
  const float norm_val = compute_norm(x, n, norm);
  const float effective_thr = std::max(threshold, constants::kEpsilon);
  if (norm_val < effective_thr) {
    return out;  // librosa's fill=None: leave unchanged
  }
  const float inv = 1.0f / norm_val;
  for (std::size_t i = 0; i < n; ++i) out[i] *= inv;
  return out;
}

std::vector<float> normalize(const std::vector<float>& x, NormType norm, float threshold) {
  return normalize(x.data(), x.size(), norm, threshold);
}

std::vector<float> normalize_matrix(const float* x, int rows, int cols, int axis, NormType norm,
                                    float threshold) {
  if (rows < 0 || cols < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "normalize_matrix: negative dimension");
  }
  const std::size_t total = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
  if (total > 0 && x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "normalize_matrix: null input");
  }
  if (axis != 0 && axis != 1) {
    throw SonareException(ErrorCode::InvalidParameter, "normalize_matrix: axis must be 0 or 1");
  }
  // See normalize(): do not construct an iterator range from a null pointer
  // when the matrix has no elements.
  if (total == 0) return {};
  std::vector<float> out(x, x + total);
  const float effective_thr = std::max(threshold, constants::kEpsilon);

  if (axis == 1) {
    // Each row is a vector of length cols.
    for (int r = 0; r < rows; ++r) {
      float* row = out.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(cols);
      const float norm_val = compute_norm(row, static_cast<std::size_t>(cols), norm);
      if (norm_val >= effective_thr) {
        const float inv = 1.0f / norm_val;
        for (int c = 0; c < cols; ++c) row[c] *= inv;
      }
    }
  } else {
    // axis == 0: each column is a vector of length rows, staged in column tiles rather than
    // gathered one column at a time, so both passes read along each row contiguously while the
    // staging stays bounded instead of scaling with the column count. Each column still takes
    // its terms in ascending row, so the norms are unchanged bit for bit.
    constexpr int kColumnTile = 256;
    std::vector<float> norms;
    std::vector<float> inv;
    std::vector<double> acc;
    for (int c0 = 0; c0 < cols; c0 += kColumnTile) {
      const int tile_cols = std::min(kColumnTile, cols - c0);
      block_column_norms(out.data(), rows, cols, c0, tile_cols, norm, norms, acc);
      inv.assign(static_cast<std::size_t>(tile_cols), 0.0f);
      for (int c = 0; c < tile_cols; ++c) {
        if (norms[c] >= effective_thr) inv[c] = 1.0f / norms[c];
      }
      for (int r = 0; r < rows; ++r) {
        float* row = out.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + c0;
        // A below-threshold column is left untouched rather than scaled by one.
        for (int c = 0; c < tile_cols; ++c) {
          if (norms[c] >= effective_thr) row[c] *= inv[c];
        }
      }
    }
  }
  return out;
}

}  // namespace sonare
