#pragma once

/// @file spectrum.h
/// @brief STFT/iSTFT and Spectrogram class.

#include <complex>
#include <functional>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "util/types.h"

namespace sonare {

/// @brief Progress callback type for iterative operations.
/// @param progress Progress value (0.0 to 1.0)
using SpectrogramProgressCallback = std::function<void(float progress)>;

/// @brief STFT output format.
enum class StftFormat {
  Complex,    ///< Complex spectrum (default)
  Magnitude,  ///< Magnitude spectrum
  Power,      ///< Power spectrum (magnitude squared)
};

/// @brief Configuration for STFT computation.
struct StftConfig {
  int n_fft = 2048;                      ///< FFT size
  int hop_length = 512;                  ///< Hop length between frames
  int win_length = 0;                    ///< Window length (0 = n_fft)
  WindowType window = WindowType::Hann;  ///< Window function
  bool center = true;                    ///< Pad signal to center frames

  /// @brief Returns actual window length (defaults to n_fft if 0).
  int actual_win_length() const { return win_length > 0 ? win_length : n_fft; }
};

/// @brief Configuration for Griffin-Lim algorithm.
/// @details Griffin-Lim iteratively estimates phase from magnitude spectrogram.
///          Momentum accelerates convergence but may cause instability if too high.
struct GriffinLimConfig {
  int n_iter = 32;         ///< Number of iterations (typically 16-64)
  float momentum = 0.99f;  ///< Momentum factor [0.0, 1.0). 0 disables, 0.99 is typical.
                           ///< Higher values converge faster but may oscillate.
};

/// @brief Spectrogram computed from audio via STFT.
/// @details Stores complex spectrum and provides views for magnitude, power, and dB.
///
/// Memory Layout:
/// - Data is stored as [n_bins x n_frames] in row-major order
/// - Access pattern: data[bin * n_frames + frame]
/// - bin index: 0 to n_bins-1 (frequency bins, n_bins = n_fft/2 + 1)
/// - frame index: 0 to n_frames-1 (time frames)
///
/// This layout is optimized for frequency-domain processing where
/// operations typically iterate over all frames for a given bin.
///
/// @note Thread safety: A single Spectrogram instance is NOT thread-safe for
///       concurrent access to magnitude()/power() methods due to lazy caching.
///       Each thread should have its own Spectrogram instance, or external
///       synchronization is required. Spectrogram::compute() itself is thread-safe.
class Spectrogram {
 public:
  /// @brief Default constructor creates empty spectrogram.
  Spectrogram();

  /// @brief Computes STFT of audio signal.
  /// @param audio Input audio
  /// @param config STFT configuration
  /// @param progress_callback Optional progress callback (0.0 to 1.0)
  /// @return Spectrogram object
  static Spectrogram compute(const Audio& audio, const StftConfig& config = StftConfig(),
                             SpectrogramProgressCallback progress_callback = nullptr);

  /// @brief Creates Spectrogram from existing complex spectrum data.
  /// @param data Complex spectrum data [n_bins x n_frames]
  /// @param n_fft Original FFT size
  /// @param hop_length Hop length used
  /// @param sample_rate Sample rate of original audio
  /// @return Spectrogram object
  static Spectrogram from_complex(const std::complex<float>* data, int n_bins, int n_frames,
                                  int n_fft, int hop_length, int sample_rate);

  /// @brief Returns number of frequency bins (n_fft/2 + 1).
  int n_bins() const { return n_bins_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns FFT size.
  int n_fft() const { return n_fft_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

  /// @brief Returns sample rate of original audio.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns true if spectrogram is empty.
  bool empty() const { return n_frames_ == 0 || n_bins_ == 0; }

  /// @brief Returns view of complex spectrum [n_bins x n_frames].
  MatrixView<std::complex<float>> complex_view() const;

  /// @brief Returns pointer to complex data.
  const std::complex<float>* complex_data() const;

  /// @brief Returns magnitude spectrum [n_bins x n_frames].
  /// @details Computed lazily and cached.
  const std::vector<float>& magnitude() const;

  /// @brief Returns power spectrum [n_bins x n_frames].
  /// @details Computed lazily and cached.
  const std::vector<float>& power() const;

  /// @brief Returns magnitude in decibels [n_bins x n_frames].
  /// @param ref Reference value (default 1.0)
  /// @param amin Minimum amplitude to avoid log(0) (default 1e-10)
  /// @return dB values
  std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

  /// @brief Reconstructs audio from spectrogram via iSTFT.
  /// @param length Target length in samples (0 = auto)
  /// @param window Window function for synthesis
  /// @return Reconstructed audio
  Audio to_audio(int length = 0, WindowType window = WindowType::Hann) const;

  /// @brief Access complex value at (bin, frame).
  const std::complex<float>& at(int bin, int frame) const;

 private:
  Spectrogram(std::vector<std::complex<float>> data, int n_bins, int n_frames, int n_fft,
              int hop_length, int sample_rate);

  std::vector<std::complex<float>> data_;  ///< Complex spectrum [n_bins * n_frames]
  int n_bins_;
  int n_frames_;
  int n_fft_;
  int hop_length_;
  int sample_rate_;

  // Cached derived data (computed lazily)
  mutable std::vector<float> magnitude_cache_;
  mutable std::vector<float> power_cache_;
};

/// @brief Reconstructs audio from magnitude spectrogram using Griffin-Lim algorithm.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param n_fft FFT size
/// @param hop_length Hop length
/// @param sample_rate Sample rate
/// @param config Griffin-Lim configuration
/// @return Reconstructed audio
Audio griffin_lim(const float* magnitude, int n_bins, int n_frames, int n_fft, int hop_length,
                  int sample_rate, const GriffinLimConfig& config = GriffinLimConfig());

/// @brief Reconstructs audio from magnitude spectrogram using Griffin-Lim algorithm.
/// @param magnitude Magnitude values as vector
/// @param n_fft FFT size
/// @param hop_length Hop length
/// @param sample_rate Sample rate
/// @param config Griffin-Lim configuration
/// @return Reconstructed audio
Audio griffin_lim(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft,
                  int hop_length, int sample_rate,
                  const GriffinLimConfig& config = GriffinLimConfig());

}  // namespace sonare
