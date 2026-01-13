#pragma once

/// @file chroma.h
/// @brief Chromagram computation for harmonic analysis.

#include <array>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "filters/chroma.h"
#include "util/types.h"

namespace sonare {

/// @brief Configuration for Chromagram computation.
struct ChromaConfig {
  // Chroma filterbank settings
  int n_chroma = 12;    ///< Number of chroma bins (typically 12)
  float tuning = 0.0f;  ///< Tuning deviation in fractions of a chroma bin
  float fmin = 0.0f;    ///< Minimum frequency (0 = C1 ~32.7 Hz)
  int n_octaves = 7;    ///< Number of octaves to span

  // STFT settings
  int n_fft = 2048;                      ///< FFT size
  int hop_length = 512;                  ///< Hop length between frames
  int win_length = 0;                    ///< Window length (0 = n_fft)
  WindowType window = WindowType::Hann;  ///< Window function
  bool center = true;                    ///< Pad signal to center frames

  /// @brief Converts to StftConfig for STFT computation.
  StftConfig to_stft_config() const {
    return StftConfig{n_fft, hop_length, win_length, window, center};
  }

  /// @brief Converts to ChromaFilterConfig for filterbank generation.
  ChromaFilterConfig to_chroma_filter_config() const {
    return ChromaFilterConfig{n_chroma, tuning, fmin, n_octaves};
  }
};

/// @brief Chromagram representation for harmonic analysis.
/// @details Computes and caches chromagram from audio or STFT.
/// Each column represents the energy distribution across 12 pitch classes.
class Chroma {
 public:
  /// @brief Default constructor creates empty chromagram.
  Chroma();

  /// @brief Computes chromagram from audio signal.
  /// @param audio Input audio
  /// @param config Chroma configuration
  /// @return Chroma object
  static Chroma compute(const Audio& audio, const ChromaConfig& config = ChromaConfig());

  /// @brief Computes chromagram from existing spectrogram.
  /// @param spec Spectrogram (STFT result)
  /// @param sr Sample rate in Hz
  /// @param chroma_config Chroma filterbank configuration
  /// @return Chroma object
  static Chroma from_spectrogram(const Spectrogram& spec, int sr,
                                 const ChromaFilterConfig& chroma_config = ChromaFilterConfig());

  /// @brief Returns number of chroma bins (typically 12).
  int n_chroma() const { return n_chroma_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns sample rate.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns hop length used.
  int hop_length() const { return hop_length_; }

  /// @brief Returns true if chromagram is empty.
  bool empty() const { return n_frames_ == 0 || n_chroma_ == 0; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns chromagram features [n_chroma x n_frames].
  MatrixView<float> features() const;

  /// @brief Returns pointer to feature data.
  const float* data() const { return features_.data(); }

  /// @brief Computes mean energy for each pitch class.
  /// @return Array of 12 mean energy values (C, C#, D, ..., B)
  std::array<float, 12> mean_energy() const;

  /// @brief Computes normalized chromagram (L1 or L2 norm per frame).
  /// @param norm Norm type: 1 for L1, 2 for L2
  /// @return Normalized chromagram [n_chroma x n_frames]
  std::vector<float> normalize(int norm = 2) const;

  /// @brief Returns the dominant pitch class for each frame.
  /// @return Vector of pitch class indices (0=C, 1=C#, ..., 11=B)
  std::vector<int> dominant_pitch_class() const;

  /// @brief Access value at (chroma, frame).
  float at(int chroma, int frame) const;

 private:
  Chroma(std::vector<float> features, int n_chroma, int n_frames, int sample_rate, int hop_length);

  std::vector<float> features_;  ///< Chromagram [n_chroma * n_frames]
  int n_chroma_;
  int n_frames_;
  int sample_rate_;
  int hop_length_;
};

}  // namespace sonare
