#pragma once

/// @file iirt.h
/// @brief Multi-rate time–frequency analysis using a semitone IIR filterbank.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Configuration for librosa.iirt.
struct IirtConfig {
  int sr = 22050;
  int hop_length = 512;
  int win_length = 2048;
  bool center = true;
  /// @brief Tuning offset (fractions of a semitone). Shifts the filterbank
  /// center frequencies by 2^(tuning/12).
  float tuning = 0.0f;
  /// @brief Number of filters (87 = full piano range starting at A0).
  int n_filters = 87;
  /// @brief MIDI note of the lowest filter (default 21 = A0).
  int midi_start = 21;
  /// @brief Filter quality factor (Q). Higher Q means narrower passbands.
  float Q = 25.0f;
  /// @brief Butterworth filter order. Must be even and >= 2. The bandpass for each
  /// band is realized as a cascade of @c filter_order/2 biquad sections whose
  /// per-section quality factors follow the maximally-flat Butterworth distribution.
  int filter_order = 2;
};

/// @brief Multi-rate energy time–frequency representation.
/// @details Applies a bank of order-@ref IirtConfig::filter_order Butterworth bandpass
/// filters (each a cascade of biquad sections) tuned to the equal-tempered
/// 12-TET scale, then computes the RMS of each band's response inside frames
/// of length @ref IirtConfig::win_length spaced @ref IirtConfig::hop_length
/// apart. Mirrors `librosa.iirt`.
/// @return Row-major matrix [n_filters x n_frames] containing RMS energies.
std::vector<float> iirt(const float* y, size_t n_samples, const IirtConfig& config = IirtConfig());

/// @brief Convenience overload accepting an Audio object.
std::vector<float> iirt(const Audio& audio, const IirtConfig& config = IirtConfig());

}  // namespace sonare
