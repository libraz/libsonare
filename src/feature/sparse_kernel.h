#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

#include "feature/cqt.h"

namespace sonare::detail {

/// Append one dense row using librosa-style cumulative L1 sparsification.
inline void append_sparsified_kernel_row(SparseComplexKernel& out, const std::complex<float>* row,
                                         int columns, float sparsity = 0.01f) {
  std::vector<int> order(static_cast<size_t>(columns));
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [row](int lhs, int rhs) { return std::abs(row[lhs]) < std::abs(row[rhs]); });

  double total = 0.0;
  for (int column = 0; column < columns; ++column) total += std::abs(row[column]);
  const double discard_budget = total * std::clamp(sparsity, 0.0f, 1.0f);
  double discarded = 0.0;
  std::vector<unsigned char> keep(static_cast<size_t>(columns), 1);
  for (int column : order) {
    const double magnitude = std::abs(row[column]);
    if (discarded + magnitude > discard_budget) break;
    keep[static_cast<size_t>(column)] = 0;
    discarded += magnitude;
  }

  for (int column = 0; column < columns; ++column) {
    if (keep[static_cast<size_t>(column)] == 0 || row[column] == std::complex<float>{}) continue;
    out.column_indices.push_back(column);
    out.values.push_back(row[column]);
  }
  out.row_offsets.push_back(static_cast<int>(out.values.size()));
}

inline std::complex<float> sparse_kernel_row_dot(const SparseComplexKernel& kernel, int row,
                                                 const std::complex<float>* vector) {
  std::complex<float> result{};
  const int begin = kernel.row_offsets[static_cast<size_t>(row)];
  const int end = kernel.row_offsets[static_cast<size_t>(row + 1)];
  for (int index = begin; index < end; ++index) {
    result += kernel.values[static_cast<size_t>(index)] *
              vector[kernel.column_indices[static_cast<size_t>(index)]];
  }
  return result;
}

}  // namespace sonare::detail
