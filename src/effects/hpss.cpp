#include "effects/hpss.h"

#include <Eigen/Core>
#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <future>
#include <thread>
#endif

#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/non_finite_sample.h"
#include "util/numeric_validation.h"

namespace sonare {

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
  /// @details Non-finite values are substituted so the sorted array remains a
  ///          strict weak ordering; a NaN-tainted input would otherwise corrupt
  ///          `std::lower_bound` and trigger undefined behavior in subsequent
  ///          erase operations. The count is discarded because the median filter
  ///          is a free-standing helper with no channel to report it on.
  void insert(float val) {
    (void)resolve_non_finite(SampleDestination::kOrderedContainer, val);
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
    (void)resolve_non_finite(SampleDestination::kOrderedContainer, val);
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
///          Non-finite entries are substituted before sorting because
///          `std::nth_element` requires a strict weak ordering. The count is
///          discarded for the same reason as in SlidingMedian.
float compute_median(float* values, size_t n) {
  if (n == 0) return 0.0f;

  (void)resolve_non_finite_run(SampleDestination::kOrderedContainer, values, n);

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

/// @brief Median-filters one contiguous line of @p n values into @p out.
/// @details Partial windows at either end take an exact median; the full-width
///          middle slides @p sm. @p window holds kernel_size floats of scratch.
void median_filter_line(const float* in, float* out, int n, int kernel_size, SlidingMedian& sm,
                        float* window) {
  const int half = kernel_size / 2;
  for (int i = 0; i < std::min(half, n); ++i) {
    const int end = std::min(i + half + 1, n);
    std::copy(in, in + end, window);
    out[i] = compute_median(window, end);
  }

  if (n > 2 * half) {
    sm.clear();
    for (int i = 0; i < kernel_size; ++i) {
      sm.insert(in[i]);
    }
    out[half] = sm.median();
    for (int i = half + 1; i < n - half; ++i) {
      sm.erase(in[i - half - 1]);
      sm.insert(in[i + half]);
      out[i] = sm.median();
    }
  }

  for (int i = std::max(half, n - half); i < n; ++i) {
    const int start = std::max(0, i - half);
    std::copy(in + start, in + n, window);
    out[i] = compute_median(window, n - start);
  }
}

#ifndef __EMSCRIPTEN__
/// @brief Executes fn(start, end) across n_workers threads over [0, total).
template <typename F>
void parallel_for(int total, int n_workers, F&& fn) {
  if (n_workers <= 1 || total <= 1) {
    fn(0, total);
    return;
  }
  n_workers = std::min(n_workers, total);
  std::vector<std::future<void>> futures;
  futures.reserve(n_workers);
  int chunk = (total + n_workers - 1) / n_workers;
  for (int i = 0; i < n_workers; ++i) {
    int start = i * chunk;
    int end = std::min(start + chunk, total);
    if (start >= end) break;
    futures.emplace_back(std::async(std::launch::async, std::forward<F>(fn), start, end));
  }
  for (auto& f : futures) f.get();
}

/// @brief Workers the host offers, as the worker-count policy takes it.
int available_workers() { return static_cast<int>(std::thread::hardware_concurrency()); }
#endif

}  // namespace

int median_filter_worker_count(int total, int kernel_size, int staged_column_length,
                               int host_concurrency) {
  if (total <= 1 || host_concurrency <= 1) return 1;
  const int offered = std::min(total, host_concurrency);

  // Two kernel-wide float arrays per worker, plus the vertical filter's staged
  // column pair. 64-bit throughout: on wasm32 the product outruns size_t.
  const uint64_t per_worker_bytes = 2u * sizeof(float) *
                                    (static_cast<uint64_t>(std::max(kernel_size, 0)) +
                                     static_cast<uint64_t>(std::max(staged_column_length, 0)));
  if (per_worker_bytes == 0) return offered;

  const uint64_t affordable = static_cast<uint64_t>(kMaxHpssScratchBytes) / per_worker_bytes;
  if (affordable >= static_cast<uint64_t>(offered)) return offered;
  return affordable == 0 ? 1 : static_cast<int>(affordable);
}

std::vector<float> median_filter_horizontal(const float* magnitude, int n_bins, int n_frames,
                                            int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);
  // Bound before the per-thread allocations below, not after. This bounds one
  // factor only -- the product is held by the worker count at the parallel_for
  // below, since each worker builds its own kernel-wide pair.
  SONARE_CHECK_MSG(kernel_size <= kMaxHpssKernelSize, ErrorCode::InvalidParameter,
                   "median_filter_horizontal: kernel_size " + std::to_string(kernel_size) +
                       " exceeds the maximum " + std::to_string(kMaxHpssKernelSize));

  std::vector<float> result(checked_spectrogram_size(n_bins, n_frames));

  auto process_rows = [&](int row_start, int row_end) {
    SlidingMedian sm(kernel_size);
    std::vector<float> window(kernel_size);

    for (int k = row_start; k < row_end; ++k) {
      const float* row = magnitude + k * n_frames;
      float* out_row = result.data() + k * n_frames;

      median_filter_line(row, out_row, n_frames, kernel_size, sm, window.data());
    }
  };

#ifndef __EMSCRIPTEN__
  // The horizontal filter stages nothing per column, so the kernel pair is its
  // whole per-worker scratch.
  parallel_for(n_bins, median_filter_worker_count(n_bins, kernel_size, 0, available_workers()),
               process_rows);
#else
  process_rows(0, n_bins);
#endif

  return result;
}

std::vector<float> median_filter_vertical(const float* magnitude, int n_bins, int n_frames,
                                          int kernel_size) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(kernel_size > 0 && kernel_size % 2 == 1, ErrorCode::InvalidParameter);
  // Bound before the per-thread allocations below, not after. This bounds one
  // factor only -- the product, kernel pair plus n_bins staging pair per worker,
  // is held by the worker count at the parallel_for below.
  SONARE_CHECK_MSG(kernel_size <= kMaxHpssKernelSize, ErrorCode::InvalidParameter,
                   "median_filter_vertical: kernel_size " + std::to_string(kernel_size) +
                       " exceeds the maximum " + std::to_string(kMaxHpssKernelSize));

  std::vector<float> result(checked_spectrogram_size(n_bins, n_frames));

  auto process_cols = [&](int col_start, int col_end) {
    SlidingMedian sm(kernel_size);
    std::vector<float> window(kernel_size);
    // The sliding window advances one bin at a time, so the loops cannot be
    // exchanged; staging the column is what takes the filter off the n_frames
    // stride. Two n_bins buffers per worker, and each magnitude element is read
    // once here instead of once per window it enters.
    std::vector<float> col(n_bins);
    std::vector<float> out_col(n_bins);

    for (int t = col_start; t < col_end; ++t) {
      for (int k = 0; k < n_bins; ++k) {
        col[k] = magnitude[k * n_frames + t];
      }

      median_filter_line(col.data(), out_col.data(), n_bins, kernel_size, sm, window.data());

      // The line filter covers every bin for any n_bins / kernel pair, so out_col
      // never carries a value over from the previous column.
      for (int k = 0; k < n_bins; ++k) {
        result[k * n_frames + t] = out_col[k];
      }
    }
  };

#ifndef __EMSCRIPTEN__
  parallel_for(n_frames,
               median_filter_worker_count(n_frames, kernel_size, n_bins, available_workers()),
               process_cols);
#else
  process_cols(0, n_frames);
#endif

  return result;
}

namespace {

/// @brief Which component a single-component HPSS pass reconstructs.
enum class HpssComponent { kHarmonic, kPercussive };

/// @brief Refuses a mask power or margin the mask expressions are not defined for.
void validate_mask_config(const HpssConfig& config) {
  SONARE_CHECK_MSG(std::isfinite(config.power) && config.power > 0.0f, ErrorCode::InvalidParameter,
                   "HPSS power must be finite and positive");
  SONARE_CHECK_MSG(std::isfinite(config.margin_harmonic) && config.margin_harmonic >= 0.0f &&
                       std::isfinite(config.margin_percussive) && config.margin_percussive >= 0.0f,
                   ErrorCode::InvalidParameter, "HPSS margins must be finite and non-negative");
}

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
  validate_mask_config(config);
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();

  /// Get magnitude spectrum
  const std::vector<float>& magnitude = spec.magnitude();

  /// Apply median filters
  std::vector<float> harmonic_enhanced =
      median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
  std::vector<float> percussive_enhanced =
      median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

  Eigen::Map<Eigen::ArrayXf> h_mag(harmonic_enhanced.data(), total_size);
  Eigen::Map<Eigen::ArrayXf> p_mag(percussive_enhanced.data(), total_size);

  if (config.use_soft_mask) {
    /// Soft masks matching librosa: the margin is applied to the *opposing*
    /// component before the power, i.e.
    ///   mask_harm = H^p / (H^p + (margin_h * P)^p)
    ///   mask_perc = P^p / (P^p + (margin_p * H)^p)
    /// so the margin contributes margin^power, not margin^1.
    const auto& h = harmonic_enhanced;
    const auto& p = percussive_enhanced;
    const double mh = config.margin_harmonic;
    const double mp = config.margin_percussive;
    for (int i = 0; i < total_size; ++i) {
      const size_t cell = static_cast<size_t>(i);
      if (harmonic_mask != nullptr) {
        (*harmonic_mask)[cell] = soft_mask(h[cell], mh * p[cell], config.power);
      }
      if (percussive_mask != nullptr) {
        (*percussive_mask)[cell] = soft_mask(p[cell], mp * h[cell], config.power);
      }
    }
  } else {
    /// Hard mask: h >= p -> harmonic=1, else percussive=1. A positive power
    /// preserves the order, so the magnitudes are compared unpowered.
    if (harmonic_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask->data(), total_size);
      h_mask = (h_mag >= p_mag).cast<float>();
    }
    if (percussive_mask != nullptr) {
      Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask->data(), total_size);
      if (harmonic_mask != nullptr) {
        Eigen::Map<const Eigen::ArrayXf> h_mask(harmonic_mask->data(), total_size);
        p_mask = 1.0f - h_mask;
      } else {
        /// The percussive mask is the harmonic one's complement, so a
        /// percussive-only pass forms the harmonic mask in place and inverts it.
        p_mask = (h_mag >= p_mag).cast<float>();
        p_mask = 1.0f - p_mask;
      }
    }
  }
}

/// @brief @p spec with every bin scaled by @p mask, as a new spectrogram.
/// @details The masked spectrum is moved into the result rather than copied, so
///          building a component costs one spectrum, not two.
Spectrogram masked_spectrogram(const Spectrogram& spec, const std::vector<float>& mask) {
  const size_t total_size = mask.size();
  std::vector<std::complex<float>> masked(total_size);
  Eigen::Map<const Eigen::ArrayXcf> complex_map(spec.complex_data(),
                                                static_cast<Eigen::Index>(total_size));
  Eigen::Map<const Eigen::ArrayXf> mask_map(mask.data(), static_cast<Eigen::Index>(total_size));
  Eigen::Map<Eigen::ArrayXcf> masked_out(masked.data(), static_cast<Eigen::Index>(total_size));
  masked_out = complex_map * mask_map;
  return Spectrogram::from_complex(std::move(masked), spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                   spec.hop_length(), spec.sample_rate(), spec.window(),
                                   spec.center(), spec.win_length());
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
  return masked_spectrogram(spec, mask);
}

}  // namespace

HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  const int total_size = static_cast<int>(checked_spectrogram_size(n_bins, n_frames));

  /// The masks are complete, and the filter scratch behind them released, before
  /// any output spectrum exists; each mask is dropped once its component is built.
  std::vector<float> harmonic_mask(total_size);
  std::vector<float> percussive_mask(total_size);
  fill_hpss_masks(spec, config, total_size, &harmonic_mask, &percussive_mask);

  HpssSpectrogramResult result;
  result.harmonic = masked_spectrogram(spec, harmonic_mask);
  std::vector<float>().swap(harmonic_mask);
  result.percussive = masked_spectrogram(spec, percussive_mask);
  return result;
}

HpssAudioResult hpss(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length, stft_config.window,
                         stft_config.actual_win_length());

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

Spectrogram harmonic(const Spectrogram& spec, const HpssConfig& config) {
  return hpss_component(spec, config, HpssComponent::kHarmonic);
}

Spectrogram percussive(const Spectrogram& spec, const HpssConfig& config) {
  return hpss_component(spec, config, HpssComponent::kPercussive);
}

Audio harmonic(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length, stft_config.window,
                         stft_config.actual_win_length());

  const Spectrogram spec = Spectrogram::compute(audio, stft_config);
  return hpss_component(spec, config, HpssComponent::kHarmonic)
      .to_audio(static_cast<int>(audio.size()));
}

Audio percussive(const Audio& audio, const HpssConfig& config, const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length, stft_config.window,
                         stft_config.actual_win_length());

  const Spectrogram spec = Spectrogram::compute(audio, stft_config);
  return hpss_component(spec, config, HpssComponent::kPercussive)
      .to_audio(static_cast<int>(audio.size()));
}

HpssSpectrogramResultWithResidual hpss_with_residual(const Spectrogram& spec,
                                                     const HpssConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  validate_mask_config(config);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();

  /// Compute masks for three-way split using Eigen
  const int total_size = static_cast<int>(checked_spectrogram_size(n_bins, n_frames));
  std::vector<float> harmonic_mask(total_size);
  std::vector<float> percussive_mask(total_size);
  std::vector<float> residual_mask(total_size);

  /// The filter scratch lives only while the masks are formed, so none of it
  /// overlaps the three output spectra built below.
  {
    const std::vector<float>& magnitude = spec.magnitude();
    std::vector<float> harmonic_enhanced =
        median_filter_horizontal(magnitude.data(), n_bins, n_frames, config.kernel_size_harmonic);
    std::vector<float> percussive_enhanced =
        median_filter_vertical(magnitude.data(), n_bins, n_frames, config.kernel_size_percussive);

    Eigen::Map<Eigen::ArrayXf> h_mask(harmonic_mask.data(), total_size);
    Eigen::Map<Eigen::ArrayXf> p_mask(percussive_mask.data(), total_size);
    Eigen::Map<Eigen::ArrayXf> r_mask(residual_mask.data(), total_size);
    const auto& h = harmonic_enhanced;
    const auto& p = percussive_enhanced;

    if (config.use_soft_mask) {
      /// The same soft masks as fill_hpss_masks.
      const double mh = config.margin_harmonic;
      const double mp = config.margin_percussive;
      for (int i = 0; i < total_size; ++i) {
        const size_t cell = static_cast<size_t>(i);
        h_mask[i] = soft_mask(h[cell], mh * p[cell], config.power);
        p_mask[i] = soft_mask(p[cell], mp * h[cell], config.power);
      }

      /// Residual is 1 - sum when margins push both masks below their full share
      Eigen::ArrayXf mask_sum = h_mask + p_mask;
      r_mask = (1.0f - mask_sum).max(0.0f);

      /// Renormalize where residual > 0
      Eigen::ArrayXf total_all = mask_sum + r_mask;
      h_mask /= total_all;
      p_mask /= total_all;
      r_mask /= total_all;
    } else {
      /// Hard mask: residual is where neither dominates clearly. H^p / P^p above 2
      /// is a share above 2/3, below 0.5 a share below 1/3; an empty cell is residual.
      for (int i = 0; i < total_size; ++i) {
        const size_t cell = static_cast<size_t>(i);
        const float share = soft_mask(h[cell], p[cell], config.power, 0.5f);
        h_mask[i] = share > 2.0f / 3.0f ? 1.0f : 0.0f;
        p_mask[i] = share < 1.0f / 3.0f ? 1.0f : 0.0f;
      }
      r_mask = 1.0f - h_mask - p_mask;
    }
  }

  /// One output spectrum is built at a time, each mask dropped behind it.
  HpssSpectrogramResultWithResidual result;
  result.harmonic = masked_spectrogram(spec, harmonic_mask);
  std::vector<float>().swap(harmonic_mask);
  result.percussive = masked_spectrogram(spec, percussive_mask);
  std::vector<float>().swap(percussive_mask);
  result.residual = masked_spectrogram(spec, residual_mask);
  return result;
}

HpssAudioResultWithResidual hpss_with_residual(const Audio& audio, const HpssConfig& config,
                                               const StftConfig& stft_config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length, stft_config.window,
                         stft_config.actual_win_length());

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
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_cola_geometry(stft_config.n_fft, stft_config.hop_length, stft_config.window,
                         stft_config.actual_win_length());

  /// Only the residual is reconstructed. Routing through the audio-level
  /// hpss_with_residual ran three inverse transforms and discarded two of them.
  /// The three MASKS are not avoidable here -- the residual is what is left
  /// after the harmonic and percussive ones, so computing it requires both.
  HpssSpectrogramResultWithResidual spec_result =
      hpss_with_residual(Spectrogram::compute(audio, stft_config), config);
  return spec_result.residual.to_audio(static_cast<int>(audio.size()));
}

}  // namespace sonare
