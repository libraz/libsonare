#include "filters/mel.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

#include "core/convert.h"
#include "util/exception.h"

namespace sonare {

std::vector<float> mel_frequencies(int n_mels, float fmin, float fmax, bool htk) {
  SONARE_CHECK(n_mels > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(fmax > fmin, ErrorCode::InvalidParameter);

  /// Convert frequency range to Mel scale
  float mel_min = htk ? hz_to_mel_htk(fmin) : hz_to_mel(fmin);
  float mel_max = htk ? hz_to_mel_htk(fmax) : hz_to_mel(fmax);

  /// Create n_mels+2 points equally spaced in Mel scale
  int n_points = n_mels + 2;
  std::vector<float> freqs(n_points);

  for (int i = 0; i < n_points; ++i) {
    float mel = mel_min + (mel_max - mel_min) * i / (n_points - 1);
    freqs[i] = htk ? mel_to_hz_htk(mel) : mel_to_hz(mel);
  }

  return freqs;
}

std::vector<float> create_mel_filterbank(int sr, int n_fft, const MelFilterConfig& config) {
  SONARE_CHECK(sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_mels > 0, ErrorCode::InvalidParameter);

  int n_bins = n_fft / 2 + 1;
  float fmax = config.fmax > 0 ? config.fmax : static_cast<float>(sr) / 2.0f;
  float fmin = config.fmin;

  SONARE_CHECK(fmax > fmin, ErrorCode::InvalidParameter);
  SONARE_CHECK(fmax <= static_cast<float>(sr) / 2.0f, ErrorCode::InvalidParameter);

  /// Get Mel frequency points
  std::vector<float> mel_freqs = mel_frequencies(config.n_mels, fmin, fmax, config.htk);

  /// Convert frequencies to FFT bin indices (can be fractional)
  std::vector<float> bin_freqs(mel_freqs.size());
  float bin_width = static_cast<float>(sr) / n_fft;
  for (size_t i = 0; i < mel_freqs.size(); ++i) {
    bin_freqs[i] = mel_freqs[i] / bin_width;
  }

  /// Create filterbank matrix [n_mels x n_bins]
  std::vector<float> filterbank(config.n_mels * n_bins, 0.0f);

  for (int m = 0; m < config.n_mels; ++m) {
    float left = bin_freqs[m];
    float center = bin_freqs[m + 1];
    float right = bin_freqs[m + 2];

    /// Create triangular filter
    for (int k = 0; k < n_bins; ++k) {
      float bin = static_cast<float>(k);

      if (bin >= left && bin <= center) {
        /// Rising edge
        if (center > left) {
          filterbank[m * n_bins + k] = (bin - left) / (center - left);
        }
      } else if (bin > center && bin <= right) {
        /// Falling edge
        if (right > center) {
          filterbank[m * n_bins + k] = (right - bin) / (right - center);
        }
      }
    }

    /// Apply Slaney normalization (area normalization)
    if (config.norm == MelNorm::Slaney) {
      float enorm = 2.0f / (mel_freqs[m + 2] - mel_freqs[m]);
      for (int k = 0; k < n_bins; ++k) {
        filterbank[m * n_bins + k] *= enorm;
      }
    }
  }

  return filterbank;
}

std::vector<float> apply_mel_filterbank(const float* power, int n_bins, int n_frames,
                                        const float* filterbank, int n_mels) {
  SONARE_CHECK(power != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(filterbank != nullptr, ErrorCode::InvalidParameter);

  // Output: [n_mels x n_frames]
  std::vector<float> mel_spec(n_mels * n_frames);

  // Use Eigen for optimized matrix multiplication
  // filterbank: [n_mels x n_bins] (row-major)
  // power: [n_bins x n_frames] (row-major)
  // result: [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> fb_map(
      filterbank, n_mels, n_bins);
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
      power_map(power, n_bins, n_frames);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> result_map(
      mel_spec.data(), n_mels, n_frames);

  // BLAS-optimized matrix multiplication
  result_map.noalias() = fb_map * power_map;

  return mel_spec;
}

}  // namespace sonare
