#pragma once

/// @file chroma.h
/// @brief Chroma filterbank generation.

#include <memory>
#include <vector>

namespace sonare {

/// @brief Filterbank column normalization strategy.
/// @details Mirrors librosa.filters.chroma's ``norm`` parameter (applied across
///          chroma bins per FFT column, axis=0). librosa's default is L2.
enum class ChromaFilterNorm {
  None,  ///< Do not normalize
  L1,    ///< Each FFT column sums to 1 across chroma bins
  L2,    ///< Each FFT column has unit Euclidean norm across chroma bins (librosa default)
};

/// @brief Configuration for Chroma filterbank.
/// @details Mirrors librosa.filters.chroma. The bank uses Gaussian bumps weighted
///          by a per-octave Gaussian envelope (ctroct/octwidth), matching the
///          librosa reference rather than a plain triangular interpolation.
struct ChromaFilterConfig {
  int n_chroma = 12;    ///< Number of chroma bins (typically 12)
  float tuning = 0.0f;  ///< Tuning deviation in fractions of a chroma bin
  float fmin = 0.0f;    ///< Unused by the STFT chroma bank (kept for ABI stability)
  int n_octaves = 7;    ///< Unused by the STFT chroma bank (a CQT-chroma concept)
  /// Per-column filter normalization. Defaults to L2 to match librosa's
  /// ``librosa.filters.chroma(norm=2)`` default.
  ChromaFilterNorm norm = ChromaFilterNorm::L2;
  /// Center of the per-octave Gaussian weighting, in octaves (librosa ctroct=5).
  float ctroct = 5.0f;
  /// Half-width of the per-octave Gaussian weighting, in octaves. <= 0 disables
  /// the octave envelope entirely (librosa octwidth default 2).
  float octwidth = 2.0f;
  /// Rotate the bank so pitch class 0 is C (librosa base_c=True). When false the
  /// bank is anchored on A, like the raw librosa.filters.chroma(base_c=False).
  bool base_c = true;
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

/// @brief Returns a cached Chroma filterbank matrix, building it on first access.
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @param config Chroma filterbank configuration (full identity is used as key)
/// @return Shared handle to the cached filterbank [n_chroma x n_bins]
/// @details Thread-safe (guarded by a mutex). Uses an LRU eviction policy
///          bounded by a small fixed capacity (see implementation). The returned
///          handle owns a reference to the immutable filterbank, so it stays
///          valid even if the entry is evicted by another thread after this call.
std::shared_ptr<const std::vector<float>> get_chroma_filterbank_cached(
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
