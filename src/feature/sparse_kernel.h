#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "feature/cqt.h"

namespace sonare::detail {

/// Append one dense row using librosa-style cumulative L1 sparsification.
inline void append_sparsified_kernel_row(SparseComplexKernel& out, const std::complex<float>* row,
                                         int columns, float sparsity = 0.01f) {
  // Zero columns are never kept and spend none of the budget, so only the
  // nonzero ones are ranked, by (magnitude, column).
  std::vector<std::pair<float, int>> order;
  order.reserve(static_cast<size_t>(columns));
  double total = 0.0;
  for (int column = 0; column < columns; ++column) {
    const float magnitude = std::abs(row[column]);
    total += magnitude;
    if (row[column] != std::complex<float>{}) order.emplace_back(magnitude, column);
  }
  const double discard_budget = total * std::clamp(sparsity, 0.0f, 1.0f);

  // The smallest columns are discarded while their running sum fits the budget, so
  // only the kept head and its boundary need ordering. Sorting every column of a
  // long kernel made the sort most of a cold kernel build.
  const auto descending = [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
    return b < a;
  };
  const size_t n = order.size();
  size_t head = std::min<size_t>(n, 1024);
  size_t kept = 0;
  while (true) {
    std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(head), order.end(),
                     descending);
    double discarded = 0.0;
    for (size_t i = head; i < n; ++i) discarded += order[i].first;
    if (discarded > discard_budget && head < n) {
      head = std::min(n, head * 2);
      continue;
    }
    std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(head), descending);
    kept = head;
    while (kept > 0 && discarded + order[kept - 1].first <= discard_budget) {
      discarded += order[kept - 1].first;
      --kept;
    }
    break;
  }

  std::vector<unsigned char> keep(static_cast<size_t>(columns), 0);
  for (size_t i = 0; i < kept; ++i) keep[static_cast<size_t>(order[i].second)] = 1;
  for (int column = 0; column < columns; ++column) {
    if (keep[static_cast<size_t>(column)] == 0) continue;
    out.column_indices.push_back(column);
    out.values.push_back(row[column]);
  }
  out.row_offsets.push_back(static_cast<int>(out.values.size()));
}

/// @brief Dot of one kernel row with the spectrum of a REAL frame of @p n_fft samples.
/// @param half_spectrum The n_fft / 2 + 1 bins a real FFT produces; a column above
///        n_fft / 2 reads the conjugate of its mirror, which is what a full complex
///        transform of the same frame would hold there.
inline std::complex<float> sparse_kernel_row_dot(const SparseComplexKernel& kernel, int row,
                                                 const std::complex<float>* half_spectrum,
                                                 int n_fft) {
  std::complex<float> result{};
  const int half = n_fft / 2;
  const int begin = kernel.row_offsets[static_cast<size_t>(row)];
  const int end = kernel.row_offsets[static_cast<size_t>(row + 1)];
  for (int index = begin; index < end; ++index) {
    const int column = kernel.column_indices[static_cast<size_t>(index)];
    const std::complex<float> x =
        column <= half ? half_spectrum[column] : std::conj(half_spectrum[n_fft - column]);
    result += kernel.values[static_cast<size_t>(index)] * x;
  }
  return result;
}

/// @brief Correlates every centre-padded frame of @p audio with a row-compressed kernel.
/// @details Shared by the CQT and VQT front ends; each row is scaled by
///          1/sqrt(raw_lengths[k]) (librosa `scale=True`). Returns the
///          [n_bins x n_frames] row-major result and writes @p n_frames.
std::vector<std::complex<float>> apply_sparse_kernel_frames(
    const Audio& audio, const SparseComplexKernel& kernel, int fft_length, int n_bins,
    const std::vector<float>& raw_lengths, int hop_length,
    const std::function<void(float)>& progress_callback, int& n_frames);

}  // namespace sonare::detail
