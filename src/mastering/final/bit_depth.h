#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::final {

struct BitDepthConfig {
  /// @brief Word length the output is quantized to, in [2, 32].
  /// @details The grid step is 2^-(target_bits - 1), with the same effective
  ///          ceiling `DitherConfig::target_bits` in dither.h documents: the
  ///          samples are `float`, so a step below binary32's own spacing
  ///          cannot be represented and settings above 25 produce the same
  ///          output as 25.
  int target_bits = 16;
  /// @brief Clamp to [-1, 1] before and after quantization.
  bool clamp = true;
};

/// @brief Quantizes @p audio to @c config.target_bits without adding dither.
/// @param[out] non_finite_samples Optional count of input samples that were not
///        finite. A NaN leaves as silence and an infinity as full scale, both in
///        range and free of any error, so this count is the only thing that
///        distinguishes such a sample from one the caller meant to deliver. The
///        clamp cannot stand in for the check: a comparison against a non-finite
///        value is false, so @c std::clamp returns it unchanged.
Audio bit_depth(const Audio& audio, const BitDepthConfig& config = {},
                size_t* non_finite_samples = nullptr);

}  // namespace sonare::mastering::final
