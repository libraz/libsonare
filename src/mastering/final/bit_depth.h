#pragma once

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
Audio bit_depth(const Audio& audio, const BitDepthConfig& config = {});

}  // namespace sonare::mastering::final
