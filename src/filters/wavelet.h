#pragma once

/// @file wavelet.h
/// @brief Wavelet / semitone filterbank and window-utility helpers.

#include <complex>
#include <vector>

namespace sonare {

/// @brief Complex Morlet (or, for `is_cqt=false`, scipy.signal.morlet2) wavelet
///        kernels at @p freqs.
/// @details Mirrors `librosa.filters.wavelet`. Returns the concatenation of
/// kernel arrays; kernel lengths are returned separately via
/// @ref wavelet_lengths.
std::vector<std::complex<float>> wavelet(const std::vector<float>& freqs, int sr,
                                         float window_param = 1.0f, bool is_cqt = true);

/// @brief Lengths of each wavelet kernel computed in @ref wavelet.
std::vector<float> wavelet_lengths(const std::vector<float>& freqs, int sr,
                                   float window_param = 1.0f);

/// @brief Semitone biquad bandpass filterbank coefficients.
/// @details Returns a flattened matrix `[(n_octaves * bins_per_octave) x 6]`
/// where each row is `(b0, b1, b2, a0, a1, a2)`.
std::vector<float> semitone_filterbank(int n_octaves = 7, int bins_per_octave = 12,
                                       float fmin = 32.7f, int sr = 22050);

/// @brief CQT-to-chroma projection matrix.
/// @return Row-major `[n_chroma x n_input]`.
std::vector<float> cq_to_chroma(int n_input, int bins_per_octave = 12, int n_chroma = 12,
                                float fmin = 0.0f);

/// @brief 2D diagonal smoothing filter.
/// @param n Kernel size (`n x n`).
/// @param direction 0 = anti-diagonal, 1 = main diagonal.
/// @param window_param Width of the Gaussian along the diagonal.
/// @return Row-major `[n x n]`.
std::vector<float> diagonal_filter(int n, int direction = 0, float window_param = 1.0f);

/// @brief Equivalent noise bandwidth of @p window (normalised so that 1.0
///        corresponds to a rectangular window).
float window_bandwidth(const std::vector<float>& window, int n = 1000);

/// @brief Sum-of-squares of @p window centered on each STFT frame.
/// @details Mirrors `librosa.filters.window_sumsquare`. Used for OLA
///          synthesis normalisation.
std::vector<float> window_sumsquare(const std::vector<float>& window, int n_frames,
                                    int hop_length, int n_fft = 0);

}  // namespace sonare
