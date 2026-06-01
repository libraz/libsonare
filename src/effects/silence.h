#pragma once

/// @file silence.h
/// @brief librosa.effects.trim / split — silence boundary detection.
/// @details Distinct from sonare::trim(const Audio&, ...) in
///          src/effects/normalize.h, which operates on Audio. These functions
///          take raw float buffers and return sample-index ranges.

#include <cstddef>
#include <utility>
#include <vector>

namespace sonare {

/// @brief Result of trim().
struct TrimResult {
  std::vector<float> audio;  ///< Trimmed audio (between start_sample and end_sample)
  int start_sample;          ///< First non-silent sample index in the original signal
  int end_sample;            ///< One past the last non-silent sample (exclusive)
};

/// @brief Trim leading and trailing silence from a mono signal.
/// @param x Input signal
/// @param n Length
/// @param top_db Signal below `top_db` below its peak RMS is considered silent.
/// @param frame_length Frame length for RMS computation
/// @param hop_length Hop length for RMS computation
/// @return TrimResult with the trimmed audio and the original-sample range.
/// @details Computes RMS per frame, finds the peak, and treats frames whose
///          RMS is below peak_dB - top_db as silent. Mirrors librosa.effects.trim.
/// @throw sonare::SonareException on null input, non-positive frame/hop, or top_db <= 0.
TrimResult trim(const float* x, std::size_t n, float top_db = 60.0f, int frame_length = 2048,
                int hop_length = 512);
TrimResult trim(const std::vector<float>& x, float top_db = 60.0f, int frame_length = 2048,
                int hop_length = 512);

/// @brief Split a signal into non-silent intervals.
/// @return Vector of (start_sample, end_sample) pairs (end exclusive). Empty
///         if the entire signal is silent.
/// @details Same RMS-based silence detection as trim().
std::vector<std::pair<int, int>> split(const float* x, std::size_t n, float top_db = 60.0f,
                                       int frame_length = 2048, int hop_length = 512);
std::vector<std::pair<int, int>> split(const std::vector<float>& x, float top_db = 60.0f,
                                       int frame_length = 2048, int hop_length = 512);

}  // namespace sonare
