#pragma once

/// @file pcen.h
/// @brief Per-Channel Energy Normalization (PCEN).

#include <vector>

namespace sonare {

/// @brief PCEN configuration matching librosa.pcen defaults.
struct PcenConfig {
  int sr = 22050;
  int hop_length = 512;
  /// Time constant of the smoothing AR(1) filter in seconds.
  float time_constant = 0.4f;
  /// Adaptive gain exponent (alpha in the librosa paper).
  float gain = 0.98f;
  /// Bias to add inside the dynamic range compression (delta).
  float bias = 2.0f;
  /// Power exponent of the compression stage (r).
  float power = 0.5f;
  /// Numerical floor used inside the (smoother + eps)^(-gain) term.
  float eps = 0.000001f;
  /// If non-empty, use this filter coefficient `b` directly instead of
  /// deriving from @ref time_constant. Length must equal 1.
  std::vector<float> b;
  /// Optional precomputed AR(1) delay-line state (Direct-Form II Transposed).
  /// If empty, the initial state defaults to `(1 - b)` for every bin, matching
  /// librosa / `scipy.signal.lfilter_zi`. Length must equal `n_bins` if set.
  std::vector<float> zi;
};

/// @brief Applies Per-Channel Energy Normalization to a spectrogram.
/// @param S Spectrogram (magnitude or power) [n_bins x n_frames] row-major.
/// @param n_bins Number of frequency bins.
/// @param n_frames Number of time frames.
/// @param config PCEN configuration.
/// @return Normalized spectrogram [n_bins x n_frames] row-major.
std::vector<float> pcen(const float* S, int n_bins, int n_frames,
                        const PcenConfig& config = PcenConfig());

/// @brief Convenience overload accepting a std::vector input.
std::vector<float> pcen(const std::vector<float>& S, int n_bins, int n_frames,
                        const PcenConfig& config = PcenConfig());

}  // namespace sonare
