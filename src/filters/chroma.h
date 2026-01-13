#pragma once

/// @file chroma.h
/// @brief Chroma filterbank generation.

#include <vector>

namespace sonare {

/// @brief Configuration for Chroma filterbank.
struct ChromaFilterConfig {
  int n_chroma = 12;    ///< Number of chroma bins (typically 12)
  float tuning = 0.0f;  ///< Tuning deviation in fractions of a chroma bin
  float fmin = 0.0f;    ///< Minimum frequency (0 = C1 ~32.7 Hz)
  int n_octaves = 7;    ///< Number of octaves to span
};

/// @brief Converts frequency to pitch class (0-11, C=0).
/// @param hz Frequency in Hz
/// @param tuning Tuning deviation in semitones
/// @return Pitch class (0=C, 1=C#, ..., 11=B), or -1 if hz <= 0
int hz_to_pitch_class(float hz, float tuning = 0.0f);

/// @brief Converts frequency to fractional pitch class.
/// @param hz Frequency in Hz
/// @param tuning Tuning deviation in semitones
/// @return Fractional pitch class [0, 12), or -1 if hz <= 0
float hz_to_chroma(float hz, float tuning = 0.0f);

/// @brief Creates Chroma filterbank matrix.
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @param config Chroma configuration
/// @return Filterbank matrix [n_chroma x n_bins] in row-major order
std::vector<float> create_chroma_filterbank(
    int sr, int n_fft, const ChromaFilterConfig& config = ChromaFilterConfig());

/// @brief Applies chroma filterbank to power spectrum.
/// @param power Power spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param filterbank Chroma filterbank [n_chroma x n_bins]
/// @param n_chroma Number of chroma bins
/// @return Chromagram [n_chroma x n_frames]
std::vector<float> apply_chroma_filterbank(const float* power, int n_bins, int n_frames,
                                           const float* filterbank, int n_chroma);

}  // namespace sonare
