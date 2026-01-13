#pragma once

/// @file mel_spectrogram.h
/// @brief Mel spectrogram and MFCC computation.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "filters/mel.h"
#include "util/types.h"

namespace sonare {

/// @brief Configuration for Mel spectrogram computation.
struct MelConfig {
  // Mel filterbank settings
  int n_mels = 128;                ///< Number of Mel bands
  float fmin = 0.0f;               ///< Minimum frequency in Hz
  float fmax = 0.0f;               ///< Maximum frequency in Hz (0 = sr/2)
  bool htk = false;                ///< Use HTK formula instead of Slaney
  MelNorm norm = MelNorm::Slaney;  ///< Normalization type

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

  /// @brief Converts to MelFilterConfig for filterbank generation.
  MelFilterConfig to_mel_filter_config() const {
    return MelFilterConfig{n_mels, fmin, fmax, htk, norm};
  }
};

/// @brief Mel spectrogram representation with optional MFCC computation.
/// @details Computes and caches Mel spectrogram from audio or STFT.
/// Provides views for power, dB, and MFCC coefficients.
class MelSpectrogram {
 public:
  /// @brief Default constructor creates empty Mel spectrogram.
  MelSpectrogram();

  /// @brief Computes Mel spectrogram from audio signal.
  /// @param audio Input audio
  /// @param config Mel spectrogram configuration
  /// @return MelSpectrogram object
  static MelSpectrogram compute(const Audio& audio, const MelConfig& config = MelConfig());

  /// @brief Computes Mel spectrogram from existing spectrogram.
  /// @param spec Spectrogram (STFT result)
  /// @param sr Sample rate in Hz
  /// @param mel_config Mel filterbank configuration
  /// @return MelSpectrogram object
  static MelSpectrogram from_spectrogram(const Spectrogram& spec, int sr,
                                         const MelFilterConfig& mel_config = MelFilterConfig());

  /// @brief Returns number of Mel bands.
  int n_mels() const { return n_mels_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns sample rate.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns hop length used.
  int hop_length() const { return hop_length_; }

  /// @brief Returns true if Mel spectrogram is empty.
  bool empty() const { return n_frames_ == 0 || n_mels_ == 0; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns Mel power spectrogram [n_mels x n_frames].
  MatrixView<float> power() const;

  /// @brief Returns pointer to power data.
  const float* power_data() const { return power_.data(); }

  /// @brief Returns Mel spectrogram in decibels [n_mels x n_frames].
  /// @param ref Reference value (default 1.0)
  /// @param amin Minimum amplitude to avoid log(0) (default 1e-10)
  /// @return dB values (computed fresh, not cached)
  std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

  /// @brief Computes MFCC coefficients [n_mfcc x n_frames].
  /// @param n_mfcc Number of MFCC coefficients to return
  /// @param lifter Liftering coefficient (0 = no liftering)
  /// @return MFCC coefficients
  /// @details Applies DCT-II to log Mel spectrogram.
  std::vector<float> mfcc(int n_mfcc = 13, float lifter = 0.0f) const;

  /// @brief Computes delta (first derivative) of features.
  /// @param features Input features [n_features x n_frames]
  /// @param n_features Number of features per frame
  /// @param width Window width for delta computation
  /// @return Delta features [n_features x n_frames]
  static std::vector<float> delta(const float* features, int n_features, int n_frames,
                                  int width = 9);

  /// @brief Access value at (mel, frame).
  float at(int mel, int frame) const;

 private:
  MelSpectrogram(std::vector<float> power, int n_mels, int n_frames, int sample_rate,
                 int hop_length);

  std::vector<float> power_;  ///< Mel power spectrogram [n_mels * n_frames]
  int n_mels_;
  int n_frames_;
  int sample_rate_;
  int hop_length_;
};

}  // namespace sonare
