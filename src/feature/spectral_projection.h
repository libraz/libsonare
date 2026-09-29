#pragma once

/// @file spectral_projection.h
/// @brief The magnitude projection between a CQT-family filter bank and an STFT
///        grid, shared by pseudo_cqt, griffinlim_cqt and griffinlim_vqt.
///
/// The projection is the filter bank's own frequency-domain basis -- the
/// sparsified, length/n_fft-scaled wavelet FFT the full transform correlates
/// against -- taken in magnitude on the one-sided grid of the kernel's FFT size.
/// That is librosa's pseudo-CQT basis (abs of __vqt_filter_fft), so every user
/// reads one scaling convention and none rescales it on its own.

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "feature/cqt.h"

namespace sonare::detail {

/// @brief |basis| of @p kernel on the one-sided grid of its FFT size.
/// @return Row-major [kernel.rows x (n_fft / 2 + 1)] magnitude projection.
inline std::vector<float> build_cqt_projection(const SparseComplexKernel& kernel, int n_fft) {
  const int n_freq = n_fft / 2 + 1;
  std::vector<float> projection(static_cast<size_t>(kernel.rows) * n_freq, 0.0f);
  for (int k = 0; k < kernel.rows; ++k) {
    const int begin = kernel.row_offsets[static_cast<size_t>(k)];
    const int end = kernel.row_offsets[static_cast<size_t>(k + 1)];
    for (int index = begin; index < end; ++index) {
      const int bin = kernel.column_indices[static_cast<size_t>(index)];
      if (bin < n_freq) {
        projection[static_cast<size_t>(k) * n_freq + bin] =
            std::abs(kernel.values[static_cast<size_t>(index)]);
      }
    }
  }
  return projection;
}

/// @brief Seeds a Hann-window STFT magnitude [n_freq x n_frames] from a
///        magnitude in the scaling cqt()/vqt() produce.
/// @details A tone of amplitude A centred on bin k reads A * sqrt(L_k) / 2 in
///          that scaling and carries (A^2 / 4) * n_fft * sum(w^2) of STFT energy
///          (3 n_fft / 8 for a periodic Hann window). Each bin is laid down in the
///          shape of its own projection row at the gain that restores that
///          energy. A tone also reaches the neighbouring bins in proportion to
///          their response, so each STFT bin is divided by the summed squared
///          (peak-normalised) response of the bins covering it; without that the
///          overlap, which widens with the bandwidth, would tilt the result.
/// @param projection Output of build_cqt_projection().
/// @param raw_lengths Fractional filter length of each bin, in samples.
inline std::vector<float> project_cqt_magnitude_to_stft(const std::vector<float>& projection,
                                                        const std::vector<float>& raw_lengths,
                                                        int n_fft, const float* magnitude,
                                                        int n_bins, int n_frames) {
  const int n_freq = n_fft / 2 + 1;
  const float hann_energy = std::sqrt(3.0f / 8.0f) * static_cast<float>(n_fft);
  std::vector<float> stft_mag(static_cast<size_t>(n_freq) * n_frames, 0.0f);
  // The CQT bin outermost gives a contiguous run of the projection and one
  // magnitude row reused across every STFT bin.
  std::vector<float> overlap(static_cast<size_t>(n_freq), 0.0f);
  for (int k = 0; k < n_bins; ++k) {
    const float* prow = projection.data() + static_cast<size_t>(k) * n_freq;
    double row_energy = 0.0;
    float row_peak = 0.0f;
    for (int b = 0; b < n_freq; ++b) {
      row_energy += static_cast<double>(prow[b]) * prow[b];
      row_peak = std::max(row_peak, prow[b]);
    }
    const float length = raw_lengths[static_cast<size_t>(k)];
    if (!(row_energy > 0.0) || !(length > 0.0f)) continue;
    const float gain =
        hann_energy / (std::sqrt(length) * static_cast<float>(std::sqrt(row_energy)));
    const float* mrow = magnitude + static_cast<size_t>(k) * n_frames;
    for (int b = 0; b < n_freq; ++b) {
      if (prow[b] == 0.0f) continue;
      const float shape = prow[b] / row_peak;
      overlap[static_cast<size_t>(b)] += shape * shape;
      const float p = prow[b] * gain;
      float* orow = stft_mag.data() + static_cast<size_t>(b) * n_frames;
      for (int t = 0; t < n_frames; ++t) orow[t] += p * mrow[t];
    }
  }
  // Only where the bank overlaps itself: an isolated bin's own tail is left as
  // laid down rather than lifted.
  for (int b = 0; b < n_freq; ++b) {
    const float cover = overlap[static_cast<size_t>(b)];
    if (!(cover > 1.0f)) continue;
    float* orow = stft_mag.data() + static_cast<size_t>(b) * n_frames;
    for (int t = 0; t < n_frames; ++t) orow[t] /= cover;
  }
  return stft_mag;
}

}  // namespace sonare::detail
