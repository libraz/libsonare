#pragma once

/// @file cqt.h
/// @brief Constant-Q Transform (CQT) for music signal analysis.

#include <complex>
#include <functional>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "util/types.h"

namespace sonare {

/// @brief Progress callback for CQT computation.
using CqtProgressCallback = std::function<void(float progress)>;

/// @brief CQT configuration.
struct CqtConfig {
  int hop_length = 512;                  ///< Hop length in samples
  float fmin = 32.7f;                    ///< Minimum frequency in Hz (C1 = 32.7 Hz)
  int n_bins = 84;                       ///< Number of frequency bins (7 octaves * 12)
  int bins_per_octave = 12;              ///< Bins per octave (12 for semitones)
  float filter_scale = 1.0f;             ///< Filter length scale factor
  WindowType window = WindowType::Hann;  ///< Window function for filters
  float sparsity = 0.01f;                ///< Sparsity threshold for kernel
};

/// @brief CQT result container.
/// @note Thread safety: A single CqtResult instance is NOT thread-safe for
///       concurrent access to magnitude()/power() methods due to lazy caching.
///       Each thread should have its own CqtResult instance, or external
///       synchronization is required. The cqt() function itself is thread-safe.
class CqtResult {
 public:
  /// @brief Default constructor creates empty result.
  CqtResult();

  /// @brief Creates CqtResult from computed data.
  CqtResult(std::vector<std::complex<float>> data, int n_bins, int n_frames,
            std::vector<float> frequencies, int hop_length, int sample_rate);

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_bins_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns hop length used.
  int hop_length() const { return hop_length_; }

  /// @brief Returns sample rate.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns true if result is empty.
  bool empty() const { return n_frames_ == 0 || n_bins_ == 0; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns view of complex CQT coefficients [n_bins x n_frames].
  MatrixView<std::complex<float>> complex_view() const;

  /// @brief Returns pointer to complex data.
  const std::complex<float>* complex_data() const { return data_.data(); }

  /// @brief Returns magnitude [n_bins x n_frames].
  const std::vector<float>& magnitude() const;

  /// @brief Returns power spectrum [n_bins x n_frames].
  const std::vector<float>& power() const;

  /// @brief Returns magnitude in decibels.
  std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

  /// @brief Returns center frequencies for each bin.
  const std::vector<float>& frequencies() const { return frequencies_; }

  /// @brief Access complex value at (bin, frame).
  const std::complex<float>& at(int bin, int frame) const;

 private:
  std::vector<std::complex<float>> data_;  ///< Complex CQT [n_bins * n_frames]
  int n_bins_ = 0;
  int n_frames_ = 0;
  int hop_length_ = 0;
  int sample_rate_ = 0;
  std::vector<float> frequencies_;

  mutable std::vector<float> magnitude_cache_;
  mutable std::vector<float> power_cache_;
};

/// @brief CQT kernel for efficient computation.
class CqtKernel {
 public:
  /// @brief Creates CQT kernel for given configuration.
  /// @param sr Sample rate
  /// @param config CQT configuration
  static std::unique_ptr<CqtKernel> create(int sr, const CqtConfig& config);

  /// @brief Returns FFT length used by kernel.
  int fft_length() const { return fft_length_; }

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_bins_; }

  /// @brief Returns center frequencies for each bin.
  const std::vector<float>& frequencies() const { return frequencies_; }

  /// @brief Returns kernel matrix in frequency domain [n_bins x fft_length].
  const std::vector<std::complex<float>>& kernel() const { return kernel_; }

  /// @brief Returns filter lengths for each bin.
  const std::vector<int>& lengths() const { return lengths_; }

 private:
  CqtKernel() = default;

  int fft_length_ = 0;
  int n_bins_ = 0;
  std::vector<float> frequencies_;
  std::vector<std::complex<float>> kernel_;  ///< [n_bins * fft_length]
  std::vector<int> lengths_;                 ///< Filter length for each bin
};

/// @brief Computes Constant-Q Transform.
/// @param audio Input audio
/// @param config CQT configuration
/// @param progress_callback Optional progress callback
/// @return CQT result
/// @note Thread-safe. Uses mutex-protected kernel cache internally.
CqtResult cqt(const Audio& audio, const CqtConfig& config = CqtConfig(),
              CqtProgressCallback progress_callback = nullptr);

/// @brief Computes pseudo-inverse CQT (reconstruction).
/// @param cqt_result CQT coefficients
/// @param length Target output length in samples (0 = auto)
/// @return Reconstructed audio
/// @warning This is a simplified reconstruction and may not produce high-quality
///          results. Consider using Griffin-Lim with CQT magnitude for better quality.
/// @deprecated Prefer using phase vocoder or Griffin-Lim based methods for
///             high-quality audio reconstruction.
[[deprecated("Use Griffin-Lim or phase vocoder for better reconstruction quality")]]
Audio icqt(const CqtResult& cqt_result, int length = 0);

/// @brief Computes CQT frequencies for given configuration.
/// @param fmin Minimum frequency
/// @param n_bins Number of bins
/// @param bins_per_octave Bins per octave
/// @return Vector of center frequencies
std::vector<float> cqt_frequencies(float fmin, int n_bins, int bins_per_octave);

/// @brief Converts CQT to chroma features.
/// @param cqt_result CQT result
/// @param n_chroma Number of chroma bins (default 12)
/// @return Chroma features [n_chroma x n_frames]
std::vector<float> cqt_to_chroma(const CqtResult& cqt_result, int n_chroma = 12);

}  // namespace sonare
