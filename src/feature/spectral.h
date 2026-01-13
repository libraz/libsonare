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

/// @brief Computes zero crossing rate of time-domain signal.
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

/// @brief Computes RMS energy of time-domain signal.
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

}  // namespace sonare
