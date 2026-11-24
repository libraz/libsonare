#pragma once

/// @file pcen.h
/// @brief Per-Channel Energy Normalization (PCEN).

#include <cstddef>
#include <string>
#include <vector>

#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare {

/// @brief The positive dimensions accepted by PCEN matrices.
inline constexpr util::check::NumericDomain kPcenMatrixDimensionDomain =
    util::check::integer_domain_of<int>(1);

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

namespace detail {
/// Validates PCEN matrix dimensions and returns their bounded element count.
inline std::size_t checked_pcen_element_count(int n_bins, int n_frames) {
  util::check::integer_within<int>(n_bins, kPcenMatrixDimensionDomain, "n_bins");
  util::check::integer_within<int>(n_frames, kPcenMatrixDimensionDomain, "n_frames");
  if (static_cast<std::size_t>(n_bins) >
      resource::kMaxOfflineAudioSamples / static_cast<std::size_t>(n_frames)) {
    util::check::refuse_relation(
        {"n_bins", "n_frames"},
        "{0} times {1} must be at most " + std::to_string(resource::kMaxOfflineAudioSamples));
  }
  return static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames);
}

/// Computes PCEN after the caller has validated dimensions and input values.
std::vector<float> pcen_with_validated_values(const float* S, int n_bins, int n_frames,
                                              std::size_t n_values, const PcenConfig& config);
}  // namespace detail

/// @brief Applies Per-Channel Energy Normalization to a spectrogram.
/// @param S Spectrogram (magnitude or power) [n_bins x n_frames] row-major.
/// @param n_bins Positive number of frequency bins.
/// @param n_frames Positive number of time frames.
/// @param config PCEN configuration.
/// @return Normalized spectrogram [n_bins x n_frames] row-major.
/// @throws SonareException(InvalidParameter) for a non-positive dimension, a non-finite
///         @p S value (named by index), a configuration out of range, or one whose gain or
///         power overflows the float range for this @p S, or when the matrix exceeds
///         @ref resource::kMaxOfflineAudioSamples elements; no output cell is non-finite.
std::vector<float> pcen(const float* S, int n_bins, int n_frames,
                        const PcenConfig& config = PcenConfig());

/// @brief Convenience overload accepting a std::vector input.
std::vector<float> pcen(const std::vector<float>& S, int n_bins, int n_frames,
                        const PcenConfig& config = PcenConfig());

}  // namespace sonare
