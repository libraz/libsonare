#include "effects/hpss.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif

#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

namespace {

/// @brief Sliding window median filter using a sorted flat array.
/// @details Uses binary search + memmove for O(log k + k) insert/erase.
///          Much better cache performance than tree-based approaches for small k.
class SlidingMedian {
 public:
  explicit SlidingMedian(int max_size) : sorted_(max_size), size_(0) {}

  /// @brief Adds a value to the window.
  void insert(float val) {
    auto pos = std::lower_bound(sorted_.begin(), sorted_.begin() + size_, val);
    int idx = static_cast<int>(pos - sorted_.begin());
    if (idx < size_) {
      std::memmove(&sorted_[idx + 1], &sorted_[idx],
                   static_cast<size_t>(size_ - idx) * sizeof(float));
    }
    sorted_[idx] = val;
    ++size_;
  }

  /// @brief Removes a value from the window.
  void erase(float val) {
    auto pos = std::lower_bound(sorted_.begin(), sorted_.begin() + size_, val);
    int idx = static_cast<int>(pos - sorted_.begin());
    --size_;
    if (idx < size_) {
      std::memmove(&sorted_[idx], &sorted_[idx + 1],
                   static_cast<size_t>(size_ - idx) * sizeof(float));
    }
  }

  /// @brief Returns the current median.
  float median() const {
    if (size_ == 0) return 0.0f;
    if (size_ % 2 == 1) {
      return sorted_[size_ / 2];
    }
    return (sorted_[size_ / 2 - 1] + sorted_[size_ / 2]) / 2.0f;
  }

  /// @brief Clears all values.
  void clear() { size_ = 0; }

 private:
  std::vector<float> sorted_;
  int size_;
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
    /// For even-sized arrays, find max of lower half
    float median_high = values[mid];
    float median_low = *std::max_element(values, values + mid);
    return (median_low + median_high) / 2.0f;
  }
  return values[mid];
}

#ifndef __EMSCRIPTEN__
/// @brief Executes fn(start, end) in parallel over [0, total).
template <typename F>
void parallel_for(int total, F&& fn) {
  int n_threads = static_cast<int>(std::thread::hardware_concurrency());
  if (n_threads <= 1 || total <= 1) {
    fn(0, total);
    return;
  }
  n_threads = std::min(n_threads, total);
  std::vector<std::thread> threads;
  threads.reserve(n_threads);
  int chunk = (total + n_threads - 1) / n_threads;
  for (int i = 0; i < n_threads; ++i) {
    int start = i * chunk;
    int end = std::min(start + chunk, total);
    if (start >= end) break;
    threads.emplace_back(std::forward<F>(fn), start, end);
  }
  for (auto& t : threads) t.join();
}
#endif

}  // namespace

std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);

  int half = kernel_size / 2;
  std::vector<float> result(n_bins * n_frames);

  auto process_rows = [&](int row_start, int row_end) {
    SlidingMedian sm(kernel_size);
    std::vector<float> window(kernel_size);

    for (int k = row_start; k < row_end; ++k) {
      const float* row = magnitude + k * n_frames;
      float* out_row = result.data() + k * n_frames;

      /// Left boundary region (partial window) - use nth_element
      for (int t = 0; t < std::min(half, n_frames); ++t) {
        int start = 0;
        int end = std::min(t + half + 1, n_frames);
        int count = end - start;
        std::copy(row + start, row + end, window.data());
        out_row[t] = compute_median(window.data(), count);
      }

      /// Middle region - use sliding window median
      if (n_frames > 2 * half) {
        sm.clear();

        /// Initialize window with first kernel_size elements
        for (int i = 0; i < kernel_size; ++i) {
          sm.insert(row[i]);
        }
        out_row[half] = sm.median();

        /// Slide window
        for (int t = half + 1; t < n_frames - half; ++t) {
          sm.erase(row[t - half - 1]);
          sm.insert(row[t + half]);
          out_row[t] = sm.median();
        }
      }

      /// Right boundary region (partial window) - use nth_element
      for (int t = std::max(half, n_frames - half); t < n_frames; ++t) {
        int start = std::max(0, t - half);
        int end = n_frames;
        int count = end - start;
        std::copy(row + start, row + end, window.data());
        out_row[t] = compute_median(window.data(), count);
      }
    }
  };

#ifndef __EMSCRIPTEN__
  parallel_for(n_bins, process_rows);
#else
  process_rows(0, n_bins);
#endif

  return result;
}

std::vector<float> median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                          int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);

  int half = kernel_size / 2;
  std::vector<float> result(n_bins * n_frames);

  auto process_cols = [&](int col_start, int col_end) {
    SlidingMedian sm(kernel_size);
    std::vector<float> window(kernel_size);

    for (int t = col_start; t < col_end; ++t) {
      /// Top boundary region (partial window) - use nth_element
      for (int k = 0; k < std::min(half, n_bins); ++k) {
        int start = 0;
        int end = std::min(k + half + 1, n_bins);
        int count = 0;
        for (int kk = start; kk < end; ++kk) {
          window[count++] = magnitude[kk * n_frames + t];
        }
        result[k * n_frames + t] = compute_median(window.data(), count);
      }

      /// Middle region - use sliding window median
      if (n_bins > 2 * half) {
        sm.clear();

        /// Initialize window with first kernel_size elements
        for (int i = 0; i < kernel_size; ++i) {
          sm.insert(magnitude[i * n_frames + t]);
        }
        result[half * n_frames + t] = sm.median();

        /// Slide window
        for (int k = half + 1; k < n_bins - half; ++k) {
          sm.erase(magnitude[(k - half - 1) * n_frames + t]);
          sm.insert(magnitude[(k + half) * n_frames + t]);
          result[k * n_frames + t] = sm.median();
        }
      }

      /// Bottom boundary region (partial window) - use nth_element
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
  };

#ifndef __EMSCRIPTEN__
  parallel_for(n_frames, process_cols);
#else
  process_cols(0, n_frames);
#endif

  return result;
}

HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  /// Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  /// Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  /// Compute masks using Eigen for vectorized power and division
  int total_size = n_bins * n_frames;
  std::vector<float> harmonic_mask(total_size);
  std::vector<float> percussive_mask(total_size);

  /// Map enhanced arrays to Eigen
  Eigen::Map<const Eigen::ArrayXf> h_enh(harmonic_enhanced.data(), total_size);
  Eigen::Map<const Eigen::ArrayXf> p_enh(percussive_enhanced.data(), total_size);

  /// Compute power using Eigen
  Eigen::ArrayXf h_pow = h_enh.pow(config.power);
  Eigen::ArrayXf p_pow = p_enh.pow(config.power);

  Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask.data(), total_size);
  Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask.data(), total_size);

  if (config.use_soft_mask) {
    /// Soft mask with margins
    Eigen::ArrayXf h_margin = h_pow * config.margin_harmonic;
    Eigen::ArrayXf p_margin = p_pow * config.margin_percussive;
    Eigen::ArrayXf total = h_margin + p_margin + kEpsilon;

    h_mask = h_margin / total;
    p_mask = p_margin / total;
  } else {
    /// Hard mask: h >= p -> harmonic=1, else percussive=1
    h_mask = (h_pow >= p_pow).cast<float>();
    p_mask = 1.0f - h_mask;
  }

  /// Apply masks to complex spectrum using Eigen
  const std::complex<float>* complex_data = spec.complex_data();

  std::vector<std::complex<float>> harmonic_complex(total_size);
  std::vector<std::complex<float>> percussive_complex(total_size);

  Eigen::Map<const Eigen::ArrayXcf> complex_map(complex_data, total_size);
  Eigen::Map<Eigen::ArrayXcf> harm_out(harmonic_complex.data(), total_size);
  Eigen::Map<Eigen::ArrayXcf> perc_out(percussive_complex.data(), total_size);

  harm_out = complex_map * h_mask;
  perc_out = complex_map * p_mask;

  /// Create result spectrograms
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

  /// Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  /// Apply HPSS
  HpssSpectrogramResult spec_result = hpss(spec, config);

  /// Convert back to audio
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

  /// Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  /// Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  /// Compute masks for three-way split using Eigen
  int total_size = n_bins * n_frames;
  std::vector<float> harmonic_mask(total_size);
  std::vector<float> percussive_mask(total_size);
  std::vector<float> residual_mask(total_size);

  /// Map enhanced arrays to Eigen
  Eigen::Map<const Eigen::ArrayXf> h_enh(harmonic_enhanced.data(), total_size);
  Eigen::Map<const Eigen::ArrayXf> p_enh(percussive_enhanced.data(), total_size);

  /// Compute power using Eigen
  Eigen::ArrayXf h_pow = h_enh.pow(config.power);
  Eigen::ArrayXf p_pow = p_enh.pow(config.power);

  Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask.data(), total_size);
  Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask.data(), total_size);
  Eigen::Map<Eigen::ArrayXf> r_mask(residual_mask.data(), total_size);

  if (config.use_soft_mask) {
    /// Soft masks with margins
    Eigen::ArrayXf h_margin = h_pow * config.margin_harmonic;
    Eigen::ArrayXf p_margin = p_pow * config.margin_percussive;
    Eigen::ArrayXf total = h_margin + p_margin + kEpsilon;

    h_mask = h_margin / total;
    p_mask = p_margin / total;

    /// Residual is 1 - sum when margins < 1
    Eigen::ArrayXf mask_sum = h_mask + p_mask;
    r_mask = (1.0f - mask_sum).max(0.0f);

    /// Renormalize where residual > 0
    Eigen::ArrayXf total_all = mask_sum + r_mask;
    h_mask /= total_all;
    p_mask /= total_all;
    r_mask /= total_all;
  } else {
    /// Hard mask: residual is where neither dominates clearly
    Eigen::ArrayXf ratio = (h_pow + kEpsilon) / (p_pow + kEpsilon);

    /// ratio > 2.0 -> harmonic only
    /// ratio < 0.5 -> percussive only
    /// else -> residual
    h_mask = (ratio > 2.0f).cast<float>();
    p_mask = (ratio < 0.5f).cast<float>();
    r_mask = 1.0f - h_mask - p_mask;
  }

  /// Apply masks to complex spectrum using Eigen
  const std::complex<float>* complex_data = spec.complex_data();

  std::vector<std::complex<float>> harmonic_complex(total_size);
  std::vector<std::complex<float>> percussive_complex(total_size);
  std::vector<std::complex<float>> residual_complex(total_size);

  Eigen::Map<const Eigen::ArrayXcf> complex_map(complex_data, total_size);
  Eigen::Map<Eigen::ArrayXcf> harm_out(harmonic_complex.data(), total_size);
  Eigen::Map<Eigen::ArrayXcf> perc_out(percussive_complex.data(), total_size);
  Eigen::Map<Eigen::ArrayXcf> res_out(residual_complex.data(), total_size);

  harm_out = complex_map * h_mask;
  perc_out = complex_map * p_mask;
  res_out = complex_map * r_mask;

  /// Create result spectrograms
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

  /// Compute STFT
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  /// Apply HPSS with residual
  HpssSpectrogramResultWithResidual spec_result = hpss_with_residual(spec, config);

  /// Convert back to audio
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
