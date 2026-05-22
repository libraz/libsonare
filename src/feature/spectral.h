#pragma once

/// @file spectral.h
/// @brief Spectral feature extraction functions.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"

namespace sonare {

/// @brief Computes spectral centroid (center of mass of spectrum).
/// @param spec Spectrogram
/// @param sr Sample rate in Hz
/// @return Spectral centroid in Hz for each frame [n_frames]
std::vector<float> spectral_centroid(const Spectrogram& spec, int sr);

/// @brief Computes spectral centroid from magnitude spectrum.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @return Spectral centroid in Hz for each frame [n_frames]
std::vector<float> spectral_centroid(const float* magnitude, int n_bins, int n_frames, int sr,
                                     int n_fft);

/// @brief Computes spectral bandwidth (weighted standard deviation around centroid).
/// @param spec Spectrogram
/// @param sr Sample rate in Hz
/// @param p Power for bandwidth calculation (default 2.0)
/// @return Spectral bandwidth in Hz for each frame [n_frames]
std::vector<float> spectral_bandwidth(const Spectrogram& spec, int sr, float p = 2.0f);

/// @brief Computes spectral bandwidth from magnitude spectrum.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @param p Power for bandwidth calculation
/// @return Spectral bandwidth in Hz for each frame [n_frames]
std::vector<float> spectral_bandwidth(const float* magnitude, int n_bins, int n_frames, int sr,
                                      int n_fft, float p = 2.0f);

/// @brief Computes spectral rolloff (frequency below which a given percentage of energy lies).
/// @param spec Spectrogram
/// @param sr Sample rate in Hz
/// @param roll_percent Percentage threshold (default 0.85 = 85%)
/// @return Rolloff frequency in Hz for each frame [n_frames]
std::vector<float> spectral_rolloff(const Spectrogram& spec, int sr, float roll_percent = 0.85f);

/// @brief Computes spectral rolloff from magnitude spectrum.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param sr Sample rate in Hz
/// @param n_fft FFT size
/// @param roll_percent Percentage threshold
/// @return Rolloff frequency in Hz for each frame [n_frames]
std::vector<float> spectral_rolloff(const float* magnitude, int n_bins, int n_frames, int sr,
                                    int n_fft, float roll_percent = 0.85f);

/// @brief Computes spectral flatness (geometric mean / arithmetic mean).
/// @param spec Spectrogram
/// @return Spectral flatness for each frame [n_frames] (0 = tonal, 1 = noise-like)
std::vector<float> spectral_flatness(const Spectrogram& spec);

/// @brief Computes spectral flatness from magnitude spectrum.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @return Spectral flatness for each frame [n_frames]
std::vector<float> spectral_flatness(const float* magnitude, int n_bins, int n_frames);

/// @brief Computes spectral contrast (difference between peaks and valleys in frequency bands).
/// @param spec Spectrogram
/// @param sr Sample rate in Hz
/// @param n_bands Number of frequency bands
/// @param fmin Minimum frequency in Hz
/// @param quantile Quantile for valley/peak detection (default 0.02)
/// @return Spectral contrast [n_bands + 1 x n_frames] (includes one band for residual)
std::vector<float> spectral_contrast(const Spectrogram& spec, int sr, int n_bands = 6,
                                     float fmin = 200.0f, float quantile = 0.02f);

/// @brief Computes polynomial coefficients fit to each frame's spectrum.
/// @param spec Spectrogram
/// @param sr Sample rate in Hz
/// @param order Polynomial order (default 1 = linear fit; coefficients high-to-low)
/// @return Row-major matrix [(order + 1) x n_frames]
std::vector<float> poly_features(const Spectrogram& spec, int sr, int order = 1);

/// @brief Computes polynomial coefficients from raw magnitude spectrum.
/// @param magnitude Magnitude or power spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param sr Sample rate in Hz
/// @param n_fft FFT size used to produce @p magnitude
/// @param order Polynomial order
/// @return Row-major matrix [(order + 1) x n_frames]
std::vector<float> poly_features(const float* magnitude, int n_bins, int n_frames, int sr,
                                 int n_fft, int order = 1);

/// @brief Computes zero crossing rate of a centered time-domain signal.
/// @param audio Input audio
/// @param frame_length Frame length in samples (default 2048)
/// @param hop_length Hop length in samples (default 512)
/// @return Zero crossing rate for each frame [n_frames]
std::vector<float> zero_crossing_rate(const Audio& audio, int frame_length = 2048,
                                      int hop_length = 512);

/// @brief Computes zero crossing rate from raw samples.
/// @param samples Audio samples
/// @param n_samples Number of samples
/// @param frame_length Frame length in samples
/// @param hop_length Hop length in samples
/// @return Zero crossing rate for each frame [n_frames]
std::vector<float> zero_crossing_rate(const float* samples, size_t n_samples, int frame_length,
                                      int hop_length);

/// @brief Computes RMS energy of a centered time-domain signal.
/// @param audio Input audio
/// @param frame_length Frame length in samples (default 2048)
/// @param hop_length Hop length in samples (default 512)
/// @return RMS energy for each frame [n_frames]
std::vector<float> rms_energy(const Audio& audio, int frame_length = 2048, int hop_length = 512);

/// @brief Computes RMS energy from raw samples.
/// @param samples Audio samples
/// @param n_samples Number of samples
/// @param frame_length Frame length in samples
/// @param hop_length Hop length in samples
/// @return RMS energy for each frame [n_frames]
std::vector<float> rms_energy(const float* samples, size_t n_samples, int frame_length,
                              int hop_length);

/// @brief Finds raw zero-crossing indices in a signal.
/// @details Mirrors `librosa.zero_crossings`. Unlike `zero_crossing_rate` which
///          returns a per-frame rate, this returns the indices `i` where the
///          sign of `y[i]` differs from `y[i-1]` (after clipping values with
///          magnitude <= `threshold` to zero).
/// @param y Input signal.
/// @param n Number of samples.
/// @param threshold Magnitudes <= threshold are treated as zero (default 1e-10).
/// @param ref_magnitude If true, scale `threshold` by `max(|y|)` (default false).
/// @param pad If true, index 0 is always reported as a zero-crossing (default true).
/// @param zero_pos If true, the sign of zero is considered positive
///        (uses `std::signbit`). If false, distinct signs {-, 0, +} are
///        compared (default true).
/// @return Sorted vector of zero-crossing indices.
/// @throw std::invalid_argument if `y` is null with `n > 0` or `threshold < 0`.
std::vector<int> zero_crossings(const float* y, size_t n, float threshold = 1e-10f,
                                bool ref_magnitude = false, bool pad = true,
                                bool zero_pos = true);
std::vector<int> zero_crossings(const std::vector<float>& y, float threshold = 1e-10f,
                                bool ref_magnitude = false, bool pad = true,
                                bool zero_pos = true);

}  // namespace sonare
