#include "effects/hpss.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <set>
#include <vector>

#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Sliding window median filter using balanced multisets.
/// @details Uses two multisets to maintain a sliding window median efficiently.
///          Complexity: O(n log k) where n is input size and k is kernel size.
///          This is significantly faster than the naive O(n*k) approach for
///          larger kernel sizes (default kernel_size=31).
class SlidingMedian {
 public:
  /// @brief Adds a value to the window.
  void insert(float val) {
    if (lo_.empty() || val <= *lo_.rbegin()) {
      lo_.insert(val);
    } else {
      hi_.insert(val);
    }
    rebalance();
  }

  /// @brief Removes a value from the window.
  void erase(float val) {
    auto it = lo_.find(val);
    if (it != lo_.end()) {
      lo_.erase(it);
    } else {
      it = hi_.find(val);
      if (it != hi_.end()) {
        hi_.erase(it);
      }
    }
    rebalance();
  }

  /// @brief Returns the current median.
  float median() const {
    if (lo_.empty()) return 0.0f;
    if (lo_.size() > hi_.size()) {
      return *lo_.rbegin();
    }
    return (*lo_.rbegin() + *hi_.begin()) / 2.0f;
  }

  /// @brief Clears all values.
  void clear() {
    lo_.clear();
    hi_.clear();
  }

 private:
  void rebalance() {
    // Keep lo_.size() >= hi_.size() and lo_.size() <= hi_.size() + 1
    while (lo_.size() > hi_.size() + 1) {
      auto it = lo_.end();
      --it;
      hi_.insert(*it);
      lo_.erase(it);
    }
    while (hi_.size() > lo_.size()) {
      auto it = hi_.begin();
      lo_.insert(*it);
      hi_.erase(it);
    }
  }

  std::multiset<float> lo_;  // Lower half (max at end)
  std::multiset<float> hi_;  // Upper half (min at begin)
};

/// @brief Computes median of values in a buffer.
/// @param values Pointer to array of values (MODIFIED by this function)
/// @param n Number of values
/// @return Median value
/// @warning This function modifies the input array via std::nth_element.
///          The array will be partially sorted after the call.
/// @details Used only for boundary regions where sliding window doesn't apply.
float compute_median(float* values, size_t n) {
  if (n == 0) return 0.0f;

  size_t mid = n / 2;
  std::nth_element(values, values + mid, values + n);

  if (n % 2 == 0) {
    // For even-sized arrays, find max of lower half
    float median_high = values[mid];
    float median_low = *std::max_element(values, values + mid);
    return (median_low + median_high) / 2.0f;
  }
  return values[mid];
}

}  // namespace

std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);

  int half = kernel_size / 2;
  std::vector<float> result(n_bins * n_frames);

  // Pre-allocate buffer for window values (boundary regions only)
  std::vector<float> window(kernel_size);

  for (int k = 0; k < n_bins; ++k) {
    const float* row = magnitude + k * n_frames;
    float* out_row = result.data() + k * n_frames;

    // Left boundary region (partial window) - use nth_element
    for (int t = 0; t < std::min(half, n_frames); ++t) {
      int start = 0;
      int end = std::min(t + half + 1, n_frames);
      int count = end - start;
      std::copy(row + start, row + end, window.data());
      out_row[t] = compute_median(window.data(), count);
    }

    // Middle region - use sliding window median O(n log k)
    if (n_frames > 2 * half) {
      SlidingMedian sm;

      // Initialize window with first kernel_size elements
      for (int i = 0; i < kernel_size; ++i) {
        sm.insert(row[i]);
      }
      out_row[half] = sm.median();

      // Slide window
      for (int t = half + 1; t < n_frames - half; ++t) {
        sm.erase(row[t - half - 1]);
        sm.insert(row[t + half]);
        out_row[t] = sm.median();
      }
    }

    // Right boundary region (partial window) - use nth_element
    for (int t = std::max(half, n_frames - half); t < n_frames; ++t) {
      int start = std::max(0, t - half);
      int end = n_frames;
      int count = end - start;
      std::copy(row + start, row + end, window.data());
      out_row[t] = compute_median(window.data(), count);
    }
  }

  return result;
}

std::vector<float> median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                          int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);

  int half = kernel_size / 2;
  std::vector<float> result(n_bins * n_frames);

  // Pre-allocate buffer for window values (boundary regions only)
  std::vector<float> window(kernel_size);

  for (int t = 0; t < n_frames; ++t) {
    // Top boundary region (partial window) - use nth_element
    for (int k = 0; k < std::min(half, n_bins); ++k) {
      int start = 0;
      int end = std::min(k + half + 1, n_bins);
      int count = 0;
      for (int kk = start; kk < end; ++kk) {
        window[count++] = magnitude[kk * n_frames + t];
      }
      result[k * n_frames + t] = compute_median(window.data(), count);
    }

    // Middle region - use sliding window median O(n log k)
    if (n_bins > 2 * half) {
      SlidingMedian sm;

      // Initialize window with first kernel_size elements
      for (int i = 0; i < kernel_size; ++i) {
        sm.insert(magnitude[i * n_frames + t]);
      }
      result[half * n_frames + t] = sm.median();

      // Slide window
      for (int k = half + 1; k < n_bins - half; ++k) {
        sm.erase(magnitude[(k - half - 1) * n_frames + t]);
        sm.insert(magnitude[(k + half) * n_frames + t]);
        result[k * n_frames + t] = sm.median();
      }
    }

    // Bottom boundary region (partial window) - use nth_element
    for (int k = std::max(half, n_bins - half); k < n_bins; ++k) {
      int start = std::max(0, k - half);
      int end = n_bins;
      int count = 0;
      for (int kk = start; kk < end; ++kk) {
        window[count++] = magnitude[kk * n_frames + t];
      }
      result[k * n_frames + t] = compute_median(window.data(), count);
    }
  }

  return result;
}

HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  // Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  // Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  // Compute masks
  std::vector<float> harmonic_mask(n_bins * n_frames);
  std::vector<float> percussive_mask(n_bins * n_frames);

  float eps = 1e-10f;

  for (int i = 0; i < n_bins * n_frames; ++i) {
    float h = std::pow(harmonic_enhanced[i], config.power);
    float p = std::pow(percussive_enhanced[i], config.power);

    if (config.use_soft_mask) {
      // Soft mask with margins
      float h_margin = h * config.margin_harmonic;
      float p_margin = p * config.margin_percussive;
      float total = h_margin + p_margin + eps;

      harmonic_mask[i] = h_margin / total;
      percussive_mask[i] = p_margin / total;
    } else {
      // Hard mask
      if (h >= p) {
        harmonic_mask[i] = 1.0f;
        percussive_mask[i] = 0.0f;
      } else {
        harmonic_mask[i] = 0.0f;
        percussive_mask[i] = 1.0f;
      }
    }
  }

  // Apply masks to complex spectrum
  const std::complex<float>* complex_data = spec.complex_data();

  std::vector<std::complex<float>> harmonic_complex(n_bins * n_frames);
  std::vector<std::complex<float>> percussive_complex(n_bins * n_frames);

  for (int i = 0; i < n_bins * n_frames; ++i) {
    harmonic_complex[i] = complex_data[i] * harmonic_mask[i];
    percussive_complex[i] = complex_data[i] * percussive_mask[i];
  }

  // Create result spectrograms
  HpssSpectrogramResult result;
  result.harmonic = Spectrogram::from_complex(harmonic_complex.data(), n_bins, n_frames,
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate());
  result.percussive =
      Spectrogram::from_complex(percussive_complex.data(), n_bins, n_frames, spec.n_fft(),
                                spec.hop_length(), spec.sample_rate());

  return result;
}

HpssAudioResult hpss(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Apply HPSS
  HpssSpectrogramResult spec_result = hpss(spec, config);

  // Convert back to audio
  HpssAudioResult result;
  result.harmonic = spec_result.harmonic.to_audio(static_cast<int>(audio.size()));
  result.percussive = spec_result.percussive.to_audio(static_cast<int>(audio.size()));

  return result;
}

Audio harmonic(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  HpssAudioResult result = hpss(audio, config, stft_config);
  return result.harmonic;
}

Audio percussive(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  HpssAudioResult result = hpss(audio, config, stft_config);
  return result.percussive;
}

HpssSpectrogramResultWithResidual hpss_with_residual(const Spectrogram& spec,
                                                     const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  // Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  // Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  // Compute masks for three-way split
  std::vector<float> harmonic_mask(n_bins * n_frames);
  std::vector<float> percussive_mask(n_bins * n_frames);
  std::vector<float> residual_mask(n_bins * n_frames);

  float eps = 1e-10f;

  for (int i = 0; i < n_bins * n_frames; ++i) {
    float h = std::pow(harmonic_enhanced[i], config.power);
    float p = std::pow(percussive_enhanced[i], config.power);

    // Soft masks with margins
    float h_margin = h * config.margin_harmonic;
    float p_margin = p * config.margin_percussive;

    if (config.use_soft_mask) {
      float total = h_margin + p_margin + eps;
      harmonic_mask[i] = h_margin / total;
      percussive_mask[i] = p_margin / total;

      // Residual is 1 - sum when margins < 1
      float sum = harmonic_mask[i] + percussive_mask[i];
      if (sum < 1.0f) {
        residual_mask[i] = 1.0f - sum;
        // Renormalize
        float total_all = sum + residual_mask[i];
        harmonic_mask[i] /= total_all;
        percussive_mask[i] /= total_all;
        residual_mask[i] /= total_all;
      } else {
        residual_mask[i] = 0.0f;
      }
    } else {
      // Hard mask: residual is where neither dominates clearly
      float ratio = (h + eps) / (p + eps);
      if (ratio > 2.0f) {
        harmonic_mask[i] = 1.0f;
        percussive_mask[i] = 0.0f;
        residual_mask[i] = 0.0f;
      } else if (ratio < 0.5f) {
        harmonic_mask[i] = 0.0f;
        percussive_mask[i] = 1.0f;
        residual_mask[i] = 0.0f;
      } else {
        harmonic_mask[i] = 0.0f;
        percussive_mask[i] = 0.0f;
        residual_mask[i] = 1.0f;
      }
    }
  }

  // Apply masks to complex spectrum
  const std::complex<float>* complex_data = spec.complex_data();

  std::vector<std::complex<float>> harmonic_complex(n_bins * n_frames);
  std::vector<std::complex<float>> percussive_complex(n_bins * n_frames);
  std::vector<std::complex<float>> residual_complex(n_bins * n_frames);

  for (int i = 0; i < n_bins * n_frames; ++i) {
    harmonic_complex[i] = complex_data[i] * harmonic_mask[i];
    percussive_complex[i] = complex_data[i] * percussive_mask[i];
    residual_complex[i] = complex_data[i] * residual_mask[i];
  }

  // Create result spectrograms
  HpssSpectrogramResultWithResidual result;
  result.harmonic = Spectrogram::from_complex(harmonic_complex.data(), n_bins, n_frames,
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate());
  result.percussive =
      Spectrogram::from_complex(percussive_complex.data(), n_bins, n_frames, spec.n_fft(),
                                spec.hop_length(), spec.sample_rate());
  result.residual = Spectrogram::from_complex(residual_complex.data(), n_bins, n_frames,
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate());

  return result;
}

HpssAudioResultWithResidual hpss_with_residual(const Audio& audio, const HpssConfig& config,
                                               const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Apply HPSS with residual
  HpssSpectrogramResultWithResidual spec_result = hpss_with_residual(spec, config);

  // Convert back to audio
  HpssAudioResultWithResidual result;
  result.harmonic = spec_result.harmonic.to_audio(static_cast<int>(audio.size()));
  result.percussive = spec_result.percussive.to_audio(static_cast<int>(audio.size()));
  result.residual = spec_result.residual.to_audio(static_cast<int>(audio.size()));

  return result;
}

Audio residual(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);
  return result.residual;
}

}  // namespace sonare
