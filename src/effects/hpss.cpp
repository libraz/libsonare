#include "effects/hpss.h"

#include <Eigen/Core>
#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <future>
#include <thread>
#endif

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

size_t checked_spectrogram_size(int n_bins, int n_frames) {
  size_t size = 0;
  SONARE_CHECK(n_bins > 0 && n_frames > 0 &&
                   numeric::checked_size_product(
                       static_cast<size_t>(n_bins), static_cast<size_t>(n_frames),
                       std::min(kMaxAudioBufferSize, static_cast<size_t>(INT_MAX)), &size),
               ErrorCode::InvalidParameter);
  return size;
}

/// @brief Sliding window median filter using a sorted flat array.
/// @details Uses binary search + memmove for O(log k + k) insert/erase.
///          Much better cache performance than tree-based approaches for small k.
class SlidingMedian {
 public:
  explicit SlidingMedian(int max_size) : sorted_(max_size), size_(0) {}

  /// @brief Adds a value to the window.
  /// @details Non-finite values (NaN / +/-Inf) are sanitized to 0.0f so that the
  ///          sorted array remains a strict weak ordering. NaN-tainted inputs
  ///          would otherwise corrupt `std::lower_bound` and trigger undefined
  ///          behavior in subsequent erase operations.
  void insert(float val) {
    if (!std::isfinite(val)) val = 0.0f;
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
  /// @details Must apply the same sanitization as `insert` so that the value
  ///          actually present in the sorted array is the one we search for.
  void erase(float val) {
    if (!std::isfinite(val)) val = 0.0f;
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
///          Non-finite entries (NaN / Inf) are sanitized to 0.0f before sorting
///          because `std::nth_element` requires a strict weak ordering.
float compute_median(float* values, size_t n) {
  if (n == 0) return 0.0f;

  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(values[i])) values[i] = 0.0f;
  }

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
  std::vector<std::future<void>> futures;
  futures.reserve(n_threads);
  int chunk = (total + n_threads - 1) / n_threads;
  for (int i = 0; i < n_threads; ++i) {
    int start = i * chunk;
    int end = std::min(start + chunk, total);
    if (start >= end) break;
    futures.emplace_back(std::async(std::launch::async, std::forward<F>(fn), start, end));
  }
  for (auto& f : futures) f.get();
}
#endif

}  // namespace

std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);

  int half = kernel_size / 2;
  std::vector<float> result(checked_spectrogram_size(n_bins, n_frames));

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
  std::vector<float> result(checked_spectrogram_size(n_bins, n_frames));

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

namespace {

/// @brief Which component a single-component HPSS pass reconstructs.
enum class HpssComponent { kHarmonic, kPercussive };

/// @brief Fills the requested separation masks from a spectrogram's magnitude.
/// @param spec Analysis spectrogram
/// @param config HPSS configuration
/// @param total_size n_bins * n_frames
/// @param harmonic_mask Harmonic mask output, or null to skip that buffer
/// @param percussive_mask Percussive mask output, or null to skip that buffer
/// @details Both masks are derived from the same pair of median-filtered
///          magnitudes, so the filters and powers are computed whichever mask
///          is asked for; passing null saves only that mask's own buffer. Both
///          the full and the single-component paths call this, so the mask
///          expressions exist once and the two cannot drift apart.
void fill_hpss_masks(const Spectrogram& spec, const HpssConfig& config, int total_size,
                     std::vector<float>* harmonic_mask, std::vector<float>* percussive_mask) {
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();

  /// Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  /// Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  /// Map enhanced arrays to Eigen
  Eigen::Map<const Eigen::ArrayXf> h_enh(harmonic_enhanced.data(), total_size);
  Eigen::Map<const Eigen::ArrayXf> p_enh(percussive_enhanced.data(), total_size);

  /// Compute power using Eigen
  Eigen::ArrayXf h_pow = h_enh.pow(config.power);
  Eigen::ArrayXf p_pow = p_enh.pow(config.power);

  if (config.use_soft_mask) {
    /// Soft masks matching librosa: the margin is applied to the *opposing*
    /// component before the power, i.e.
    ///   mask_harm = H^p / (H^p + (margin_h * P)^p)
    ///   mask_perc = P^p / (P^p + (margin_p * H)^p)
    /// Since margin is inside the power, it contributes margin^power (not the
    /// margin^1 that a post-power multiply would give).
    const float mh_p = std::pow(config.margin_harmonic, config.power);
    const float mp_p = std::pow(config.margin_percussive, config.power);

    if (harmonic_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask->data(), total_size);
      h_mask = h_pow / (h_pow + mh_p * p_pow + kEpsilon);
    }
    if (percussive_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask->data(), total_size);
      p_mask = p_pow / (p_pow + mp_p * h_pow + kEpsilon);
    }
  } else {
    /// Hard mask: h >= p -> harmonic=1, else percussive=1
    if (harmonic_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask->data(), total_size);
      h_mask = (h_pow >= p_pow).cast<float>();
    }
    if (percussive_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask->data(), total_size);
      if (harmonic_mask != nullptr) {
        Eigen::Map<const Eigen::ArrayXf> h_mask(harmonic_mask->data(), total_size);
        p_mask = 1.0f - h_mask;
      } else {
        /// The percussive mask is the harmonic one's complement, so a
        /// percussive-only pass forms the harmonic mask in place and inverts it.
        p_mask = (h_pow >= p_pow).cast<float>();
        p_mask = 1.0f - p_mask;
      }
    }
  }
}

/// @brief Reconstructs one HPSS component's spectrogram, and only that one.
/// @param spec Analysis spectrogram
/// @param config HPSS configuration
/// @param want Component to reconstruct
/// @return Masked spectrogram for `want`
/// @details Same mask and same masked spectrum as the two-component path, so
///          the result is bit-identical to taking one field of that result.
///          What it avoids is the discarded component's mask, its masked
///          complex spectrum and its reconstruction, which for a large STFT
///          are the biggest buffers the separation touches.
///
///          Deliberately does NOT free the analysis spectrogram before
///          reconstructing. Doing so lowers peak live bytes but leaves a hole
///          the allocator does not reuse for the differently sized
///          reconstruction, and against a fixed heap ceiling the extra growth
///          costs more than the saving returns.
Spectrogram hpss_component(const Spectrogram& spec, const HpssConfig& config, HpssComponent want) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const int total_size = static_cast<int>(checked_spectrogram_size(n_bins, n_frames));

  std::vector<float> mask(total_size);
  fill_hpss_masks(spec, config, total_size, want == HpssComponent::kHarmonic ? &mask : nullptr,
                  want == HpssComponent::kPercussive ? &mask : nullptr);

  std::vector<std::complex<float>> masked(total_size);
  Eigen::Map<const Eigen::ArrayXcf> complex_map(spec.complex_data(), total_size);
  Eigen::Map<const Eigen::ArrayXf> mask_map(mask.data(), total_size);
  Eigen::Map<Eigen::ArrayXcf> masked_out(masked.data(), total_size);
  masked_out = complex_map * mask_map;

  return Spectrogram::from_complex(masked.data(), n_bins, n_frames, spec.n_fft(), spec.hop_length(),
                                   spec.sample_rate(), spec.window(), spec.center(),
                                   spec.win_length());
}

}  // namespace

HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  const int total_size = static_cast<int>(checked_spectrogram_size(n_bins, n_frames));

  std::vector<std::complex<float>> harmonic_complex(total_size);
  std::vector<std::complex<float>> percussive_complex(total_size);

  {
    /// Both masks are dead once they have been applied, so they do not outlive
    /// this scope and overlap the two reconstructions below.
    std::vector<float> harmonic_mask(total_size);
    std::vector<float> percussive_mask(total_size);
    fill_hpss_masks(spec, config, total_size, &harmonic_mask, &percussive_mask);

    Eigen::Map<const Eigen::ArrayXf> h_mask(harmonic_mask.data(), total_size);
    Eigen::Map<const Eigen::ArrayXf> p_mask(percussive_mask.data(), total_size);

    /// Apply masks to complex spectrum using Eigen
    Eigen::Map<const Eigen::ArrayXcf> complex_map(spec.complex_data(), total_size);
    Eigen::Map<Eigen::ArrayXcf> harm_out(harmonic_complex.data(), total_size);
    Eigen::Map<Eigen::ArrayXcf> perc_out(percussive_complex.data(), total_size);

    harm_out = complex_map * h_mask;
    perc_out = complex_map * p_mask;
  }

  /// Create result spectrograms
  HpssSpectrogramResult result;
  result.harmonic = Spectrogram::from_complex(harmonic_complex.data(), n_bins, n_frames,
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate(),
                                              spec.window(), spec.center(), spec.win_length());
  result.percussive = Spectrogram::from_complex(percussive_complex.data(), n_bins, n_frames,
                                                spec.n_fft(), spec.hop_length(), spec.sample_rate(),
                                                spec.window(), spec.center(), spec.win_length());

  return result;
}

HpssAudioResult hpss(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length);

  /// Compute the STFT and separate it. The analysis spectrogram is passed as a
  /// temporary so it and its magnitude cache are released when this statement
  /// ends, rather than staying alive across the two inverse transforms below.
  HpssSpectrogramResult spec_result = hpss(Spectrogram::compute(audio, stft_config), config);

  /// Convert back to audio
  HpssAudioResult result;
  result.harmonic = spec_result.harmonic.to_audio(static_cast<int>(audio.size()));
  result.percussive = spec_result.percussive.to_audio(static_cast<int>(audio.size()));

  return result;
}

Audio harmonic(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length);

  const Spectrogram spec = Spectrogram::compute(audio, stft_config);
  return hpss_component(spec, config, HpssComponent::kHarmonic)
      .to_audio(static_cast<int>(audio.size()));
}

Audio percussive(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length);

  const Spectrogram spec = Spectrogram::compute(audio, stft_config);
  return hpss_component(spec, config, HpssComponent::kPercussive)
      .to_audio(static_cast<int>(audio.size()));
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
  const int total_size = static_cast<int>(checked_spectrogram_size(n_bins, n_frames));
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
    /// Soft masks matching librosa: the margin is applied to the *opposing*
    /// component before the power (see hpss() above for the derivation), so the
    /// margin contributes margin^power rather than margin^1.
    const float mh_p = std::pow(config.margin_harmonic, config.power);
    const float mp_p = std::pow(config.margin_percussive, config.power);

    h_mask = h_pow / (h_pow + mh_p * p_pow + kEpsilon);
    p_mask = p_pow / (p_pow + mp_p * h_pow + kEpsilon);

    /// Residual is 1 - sum when margins push both masks below their full share
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
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate(),
                                              spec.window(), spec.center(), spec.win_length());
  result.percussive = Spectrogram::from_complex(percussive_complex.data(), n_bins, n_frames,
                                                spec.n_fft(), spec.hop_length(), spec.sample_rate(),
                                                spec.window(), spec.center(), spec.win_length());
  result.residual = Spectrogram::from_complex(residual_complex.data(), n_bins, n_frames,
                                              spec.n_fft(), spec.hop_length(), spec.sample_rate(),
                                              spec.window(), spec.center(), spec.win_length());

  return result;
}

HpssAudioResultWithResidual hpss_with_residual(const Audio& audio, const HpssConfig& config,
                                               const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length);

  /// Compute the STFT and separate it. Passing the analysis spectrogram as a
  /// temporary releases it and its magnitude cache when this statement ends,
  /// rather than holding them across the three inverse transforms below.
  HpssSpectrogramResultWithResidual spec_result =
      hpss_with_residual(Spectrogram::compute(audio, stft_config), config);

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
