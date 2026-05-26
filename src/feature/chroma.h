#pragma once

/// @file chroma.h
/// @brief Chromagram computation for harmonic analysis.

#include <array>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/cqt.h"
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

/// @brief Configuration for chroma_cqt / chroma_cens.
struct ChromaCqtConfig {
  CqtConfig cqt;
  int n_chroma = 12;
  float tuning = 0.0f;     ///< Tuning deviation in fractional semitones/chroma bins.
  float threshold = 0.0f;  ///< Magnitudes below this fraction of the per-frame max are zeroed.
  bool normalize_frames = true;  ///< L-inf normalize per frame (matches librosa default norm=Inf).
};

/// @brief Configuration for chroma_cens (Energy Normalized Statistics).
struct ChromaCensConfig {
  ChromaCqtConfig base;
  int win_len_smooth = 41;  ///< Hann smoothing window length (0 disables smoothing).
};

/// @brief Configuration for low-frequency bass chroma extraction.
struct BassChromaConfig {
  CqtConfig cqt = [] {
    CqtConfig config;
    config.fmin = 41.203f;  // E1
    config.n_bins = 108;    // E1-D#4 at three bins per semitone
    config.bins_per_octave = 36;
    return config;
  }();
  int n_chroma = 12;
  float tuning = 0.0f;
  bool normalize_frames = true;
};

class Chroma;  // forward declaration; full definition below.

/// @brief Computes a chromagram from a Constant-Q Transform of the audio.
/// @details Implements `librosa.feature.chroma_cqt`. The signal is mapped to
/// CQT, magnitudes are wrapped to chroma classes, optionally thresholded, then
/// L-inf normalized per frame.
Chroma chroma_cqt(const Audio& audio, const ChromaCqtConfig& config = ChromaCqtConfig());

/// @brief Computes CENS (Chroma Energy Normalized Statistics).
/// @details Implements `librosa.feature.chroma_cens`. Builds chroma_cqt, then
/// applies quantization steps and an optional Hann smoothing window across time.
Chroma chroma_cens(const Audio& audio, const ChromaCensConfig& config = ChromaCensConfig());

/// @brief Computes a low-frequency chromagram intended for bass/inversion estimation.
Chroma bass_chroma(const Audio& audio, const BassChromaConfig& config = BassChromaConfig());

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

  /// @brief Computes frame-weighted mean energy for each pitch class.
  /// @param frame_weights Non-negative weights indexed by chroma frame
  /// @return Array of 12 weighted mean energy values
  std::array<float, 12> weighted_mean_energy(const std::vector<float>& frame_weights) const;

  /// @brief Computes normalized chromagram per frame.
  /// @param norm Norm type: 0 for max (inf), 1 for L1, 2 for L2
  /// @return Normalized chromagram [n_chroma x n_frames]
  /// @note norm=0 uses max norm
  std::vector<float> normalize(int norm = 0) const;

  /// @brief Returns the dominant pitch class for each frame.
  /// @return Vector of pitch class indices (0=C, 1=C#, ..., 11=B)
  std::vector<int> dominant_pitch_class() const;

  /// @brief Access value at (chroma, frame).
  float at(int chroma, int frame) const;

  /// @brief Constructs Chroma from pre-computed features.
  /// @param features Chroma features [n_chroma * n_frames]
  /// @param n_chroma Number of chroma bins (typically 12)
  /// @param n_frames Number of time frames
  /// @param sample_rate Sample rate in Hz
  /// @param hop_length Hop length used for computation
  Chroma(std::vector<float> features, int n_chroma, int n_frames, int sample_rate, int hop_length);

 private:
  std::vector<float> features_;  ///< Chromagram [n_chroma * n_frames]
  int n_chroma_;
  int n_frames_;
  int sample_rate_;
  int hop_length_;
};

}  // namespace sonare
