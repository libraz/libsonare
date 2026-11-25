#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "feature/cqt.h"

namespace sonare::detail {

// Unique column indices break magnitude ties in both ranking passes.
[[gnu::noinline]] inline size_t rank_sparse_columns(std::vector<std::pair<float, int>>& order,
                                                    double budget, double& discarded) {
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return b < a; });
  size_t kept = order.size();
  while (kept > 0 && discarded + order[kept - 1].first <= budget) {
    discarded += order[kept - 1].first;
    --kept;
  }
  return kept;
}

/// @brief Appends one dense row using librosa-style cumulative L1 sparsification.
/// @details The smallest columns, ranked by (magnitude, column), are discarded while their
///          running sum fits `sparsity` of the row's L1 norm. Which columns that is does not
///          depend on the order the discarded ones are summed in: a decision within summation
///          rounding of the budget is retaken in full ascending order.
inline void append_sparsified_kernel_row(SparseComplexKernel& out, const std::complex<float>* row,
                                         int columns, float sparsity = 0.01f) {
  // Magnitudes are binned by their leading bits (eight bins per octave) so that only the
  // columns near and above the discard boundary are ever ranked.
  constexpr int kBucketShift = 20;
  constexpr size_t kBuckets = size_t{1} << (32 - kBucketShift);
  const auto bucket_of = [](float magnitude) {
    uint32_t bits = 0;
    std::memcpy(&bits, &magnitude, sizeof(bits));
    return static_cast<size_t>(bits >> kBucketShift);
  };

  // Zero columns are never kept and spend none of the budget.
  std::vector<float> magnitudes(static_cast<size_t>(columns));
  std::vector<double> bucket_sum(kBuckets, 0.0);
  size_t n = 0;
  double total = 0.0;
  for (int column = 0; column < columns; ++column) {
    const float magnitude = std::abs(row[column]);
    total += magnitude;
    magnitudes[static_cast<size_t>(column)] = magnitude;
    if (row[column] != std::complex<float>{}) {
      ++n;
      bucket_sum[bucket_of(magnitude)] += magnitude;
    }
  }
  const double discard_budget = total * std::clamp(sparsity, 0.0f, 1.0f);

  // Whole bins are discarded from the smallest up; the first one that does not fit holds
  // the boundary, so it and everything above it are ranked.
  double discarded = 0.0;
  size_t boundary = 0;
  while (boundary < kBuckets && discarded + bucket_sum[boundary] <= discard_budget) {
    discarded += bucket_sum[boundary];
    ++boundary;
  }
  std::vector<std::pair<float, int>> order;
  for (int column = 0; column < columns; ++column) {
    const float magnitude = magnitudes[static_cast<size_t>(column)];
    if (bucket_of(magnitude) >= boundary && row[column] != std::complex<float>{}) {
      order.emplace_back(magnitude, column);
    }
  }
  size_t kept = rank_sparse_columns(order, discard_budget, discarded);

  // The discarded columns were summed bin by bin, not in rank order.
  const double rounding_margin =
      static_cast<double>(n + 1) * std::ldexp(1.0, -52) * std::max(total, discard_budget);
  const bool near_boundary =
      (kept < n && discard_budget - discarded <= rounding_margin) ||
      (kept > 0 && discarded + order[kept - 1].first - discard_budget <= rounding_margin);
  if (near_boundary) {
    order.clear();
    for (int column = 0; column < columns; ++column) {
      if (row[column] != std::complex<float>{}) {
        order.emplace_back(magnitudes[static_cast<size_t>(column)], column);
      }
    }
    double ascending = 0.0;
    kept = rank_sparse_columns(order, discard_budget, ascending);
  }

  std::vector<int> kept_columns(kept);
  for (size_t i = 0; i < kept; ++i) kept_columns[i] = order[i].second;
  std::sort(kept_columns.begin(), kept_columns.end());
  for (const int column : kept_columns) {
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
