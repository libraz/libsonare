#pragma once

/// @file quantize_report.h
/// @brief What a conversion onto a fixed-point grid did to the samples it could not carry.

#include <cstddef>

namespace sonare {

/// @brief Samples a quantizing stage (a PCM writer, the bit-depth and dither stages) could not
///        carry onto its grid as they were, summed over every channel it converted.
struct QuantizeReport {
  /// Input samples that were not finite. Each leaves as digital silence, which is in range and
  /// free of any error, so this count is the only thing that tells it from a sample the caller
  /// meant to deliver.
  std::size_t non_finite_samples = 0;
  /// Finite input samples past full scale (|x| > 1) before quantization and dither, each held at
  /// the end of the code range. The top code is one step under +1.0, so a sample of exactly 1.0
  /// landing one step low, or a sample under full scale that dither carries onto the top code, is
  /// the grid and the noise and is not counted: a non-zero count always means signal was lost.
  std::size_t clipped_samples = 0;
};

}  // namespace sonare
