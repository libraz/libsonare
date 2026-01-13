#pragma once

/// @file vqt.h
/// @brief Variable-Q Transform (VQT) for music signal analysis.
/// @details VQT extends CQT with variable Q factor controlled by gamma parameter.
/// When gamma=0, VQT is equivalent to CQT.

#include <functional>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "feature/cqt.h"

namespace sonare {

/// @brief Progress callback for VQT computation.
using VqtProgressCallback = std::function<void(float progress)>;

/// @brief VQT configuration.
struct VqtConfig {
  int hop_length = 512;                  ///< Hop length in samples
  float fmin = 32.7f;                    ///< Minimum frequency in Hz (C1)
  float fmax = 0.0f;                     ///< Maximum frequency (0 = auto from n_bins)
  int n_bins = 84;                       ///< Number of frequency bins
  int bins_per_octave = 12;              ///< Bins per octave
  float gamma = 0.0f;                    ///< Bandwidth offset (0 = standard CQT)
  float filter_scale = 1.0f;             ///< Filter length scale factor
  WindowType window = WindowType::Hann;  ///< Window function for filters

  /// @brief Converts to CQT config (for gamma=0 case).
  CqtConfig to_cqt_config() const;
};

/// @brief VQT result (same structure as CQT).
using VqtResult = CqtResult;

/// @brief VQT kernel for efficient computation.
class VqtKernel {
 public:
  /// @brief Creates VQT kernel for given configuration.
  /// @param sr Sample rate
  /// @param config VQT configuration
  static std::unique_ptr<VqtKernel> create(int sr, const VqtConfig& config);

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

  /// @brief Returns bandwidths for each bin.
  const std::vector<float>& bandwidths() const { return bandwidths_; }

 private:
  VqtKernel() = default;

  int fft_length_ = 0;
  int n_bins_ = 0;
  std::vector<float> frequencies_;
  std::vector<float> bandwidths_;
  std::vector<std::complex<float>> kernel_;
  std::vector<int> lengths_;
};

/// @brief Computes Variable-Q Transform.
/// @param audio Input audio
/// @param config VQT configuration
/// @param progress_callback Optional progress callback
/// @return VQT result
VqtResult vqt(const Audio& audio, const VqtConfig& config = VqtConfig(),
              VqtProgressCallback progress_callback = nullptr);

/// @brief Computes pseudo-inverse VQT (reconstruction).
/// @param vqt_result VQT coefficients
/// @param length Target output length in samples (0 = auto)
/// @return Reconstructed audio
Audio ivqt(const VqtResult& vqt_result, int length = 0);

/// @brief Computes VQT frequencies for given configuration.
/// @param fmin Minimum frequency
/// @param n_bins Number of bins
/// @param bins_per_octave Bins per octave
/// @return Vector of center frequencies
std::vector<float> vqt_frequencies(float fmin, int n_bins, int bins_per_octave);

/// @brief Computes VQT bandwidths for given configuration.
/// @param frequencies Center frequencies
/// @param bins_per_octave Bins per octave
/// @param gamma Bandwidth offset
/// @return Vector of bandwidths
std::vector<float> vqt_bandwidths(const std::vector<float>& frequencies, int bins_per_octave,
                                  float gamma);

}  // namespace sonare
