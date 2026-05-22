/// @file vector_normalize.cpp
/// @brief Implementation of vector / matrix normalization.

#include "util/vector_normalize.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"

namespace sonare {

namespace {

float compute_norm(const float* x, std::size_t n, NormType type) {
  if (n == 0) return 0.0f;
  switch (type) {
    case NormType::Inf: {
      float m = 0.0f;
      for (std::size_t i = 0; i < n; ++i) m = std::max(m, std::abs(x[i]));
      return m;
    }
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

}  // namespace

std::vector<float> normalize(const float* x, std::size_t n, NormType norm, float threshold) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("normalize: null input with non-zero length");
  }
  std::vector<float> out(x, x + n);
  if (n == 0) return out;
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
    throw std::invalid_argument("normalize_matrix: negative dimension");
  }
  const std::size_t total = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
  if (total > 0 && x == nullptr) {
    throw std::invalid_argument("normalize_matrix: null input");
  }
  if (axis != 0 && axis != 1) {
    throw std::invalid_argument("normalize_matrix: axis must be 0 or 1");
  }
  std::vector<float> out(x, x + total);
  if (total == 0) return out;
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
    // axis == 0: each column is a vector of length rows.
    std::vector<float> tmp(static_cast<std::size_t>(rows));
    for (int c = 0; c < cols; ++c) {
      for (int r = 0; r < rows; ++r) {
        tmp[static_cast<std::size_t>(r)] =
            out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(c)];
      }
      const float norm_val = compute_norm(tmp.data(), static_cast<std::size_t>(rows), norm);
      if (norm_val >= effective_thr) {
        const float inv = 1.0f / norm_val;
        for (int r = 0; r < rows; ++r) {
          out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
              static_cast<std::size_t>(c)] *= inv;
        }
      }
    }
  }
  return out;
}

}  // namespace sonare
