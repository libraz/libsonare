#pragma once

/// @file mel.h
/// @brief Mel filterbank generation.

#include <vector>

namespace sonare {

/// @brief Mel filterbank normalization type.
enum class MelNorm {
  None,   ///< No normalization
  Slaney  ///< Slaney-style area normalization (librosa default)
};

/// @brief Configuration for Mel filterbank.
struct MelFilterConfig {
  int n_mels = 128;                ///< Number of Mel bands
  float fmin = 0.0f;               ///< Minimum frequency in Hz
  float fmax = 0.0f;               ///< Maximum frequency in Hz (0 = sr/2)
  bool htk = false;                ///< Use HTK formula instead of Slaney
  MelNorm norm = MelNorm::Slaney;  ///< Normalization type
};

/// @brief Computes Mel frequency points.
/// @param n_mels Number of Mel bands
/// @param fmin Minimum frequency in Hz
/// @param fmax Maximum frequency in Hz
/// @param htk Use HTK formula if true
/// @return Vector of n_mels+2 frequency points in Hz
std::vector<float> mel_frequencies(int n_mels, float fmin, float fmax, bool htk = false);

/// @brief Creates Mel filterbank matrix.
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @param config Mel filterbank configuration
/// @return Filterbank matrix [n_mels x n_bins] in row-major order
/// @details Each row is a triangular filter centered at a Mel frequency.
std::vector<float> create_mel_filterbank(int sr, int n_fft, const MelFilterConfig& config);

/// @brief Applies Mel filterbank to power spectrum.
/// @param power Power spectrum [n_bins x n_frames] in row-major order
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param filterbank Mel filterbank [n_mels x n_bins]
/// @param n_mels Number of Mel bands
/// @return Mel spectrogram [n_mels x n_frames]
std::vector<float> apply_mel_filterbank(const float* power, int n_bins, int n_frames,
                                        const float* filterbank, int n_mels);

}  // namespace sonare
