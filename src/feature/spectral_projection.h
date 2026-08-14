#pragma once

/// @file spectral_projection.h
/// @brief Shared spectral projection helpers used by CQT and VQT inversion.

#include <algorithm>
#include <cmath>
#include <vector>

#include "feature/cqt.h"
#include "util/math_utils.h"

namespace sonare::detail {

/// @brief Picks an FFT size for a CQT-shaped spectral projection.
/// @details The projection uses the CQT baseline even for VQT. VQT widens
/// low-frequency bands, so this size remains sufficient while preserving the
/// established CQT reconstruction grid.
inline int choose_pseudo_cqt_nfft(const CqtConfig& config, int sr) {
  const float q = config.filter_scale /
                  (std::pow(2.0f, 1.0f / static_cast<float>(config.bins_per_octave)) - 1.0f);
  const int max_filter_len = static_cast<int>(std::ceil(q * sr / std::max(config.fmin, 1.0f)));
  int n_fft = next_power_of_2(std::max(max_filter_len, 32));
  // Cap to avoid huge FFTs at extreme fmin values.
  n_fft = std::min(n_fft, 16384);
  return n_fft;
}

/// @brief Builds a Gaussian magnitude projection from arbitrary band widths.
/// @param freqs Center frequency for each projected bin.
/// @param bandwidths Gaussian width for each projected bin, in Hz.
/// @param n_freq Number of one-sided FFT bins.
/// @param bin_to_hz Frequency spacing between FFT bins.
/// @return Row-major projection matrix [n_bins x n_freq], with each row
///         normalized to sum to one.
inline std::vector<float> build_cqt_projection(const std::vector<float>& freqs,
                                               const std::vector<float>& bandwidths, int n_freq,
                                               float bin_to_hz) {
  const int n_bins = static_cast<int>(freqs.size());
  std::vector<float> projection(static_cast<size_t>(n_bins) * n_freq, 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    const float f = freqs[k];
    const float bandwidth = std::max(bandwidths[k], bin_to_hz);
    float row_sum = 0.0f;
    for (int b = 0; b < n_freq; ++b) {
      const float hz = static_cast<float>(b) * bin_to_hz;
      const float d = (hz - f) / bandwidth;
      const float value = std::exp(-0.5f * d * d);
      projection[k * n_freq + b] = value;
      row_sum += value;
    }
    if (row_sum > 0.0f) {
      for (int b = 0; b < n_freq; ++b) projection[k * n_freq + b] /= row_sum;
    }
  }
  return projection;
}

}  // namespace sonare::detail
