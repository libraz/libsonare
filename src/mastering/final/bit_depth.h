#pragma once

#include <cstddef>

#include "core/audio.h"
#include "core/quantize_report.h"

namespace sonare::mastering::final {

struct BitDepthConfig {
  /// @brief Word length the output is quantized to, in [2, 32].
  /// @details The grid step is 2^-(target_bits - 1), with the same effective
  ///          ceiling `DitherConfig::target_bits` in dither.h documents: the
  ///          samples are `float`, so a step below binary32's own spacing
  ///          cannot be represented and settings above 25 produce the same
  ///          output as 25.
  int target_bits = 16;
};

/// @brief Quantizes @p audio to @c config.target_bits without adding dither.
/// @details Every sample saturates to the word's code range [-1, 1 - 2^-(target_bits - 1)].
/// @param[out] report Optional counts of the non-finite samples (each leaves as silence) and of
///        the finite ones past full scale the code range held (see @ref QuantizeReport). The
///        code clamp cannot stand in for the non-finite check: a comparison against a non-finite
///        value is false, so @c std::clamp returns it unchanged.
Audio bit_depth(const Audio& audio, const BitDepthConfig& config = {},
                QuantizeReport* report = nullptr);

}  // namespace sonare::mastering::final
