#pragma once

#include <cstddef>

#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"

namespace sonare::mastering::final {

struct OutputChainConfig {
  int target_bits = 16;
  DitherType dither_type = DitherType::Tpdf;
  /// @brief See BitDepthConfig::clamp: the code range already bounds the output.
  bool clamp = true;
};

/// @brief Quantizes @p audio to @c config.target_bits through exactly one stage.
/// @details @c DitherType::None runs @ref bit_depth (honouring @c config.clamp);
///          any other type runs @ref dither alone, whose output already sits on
///          the target grid inside the code range, so @c config.clamp has no
///          effect there.
/// @param[out] non_finite_samples Optional count of input samples that were not
///        finite, as reported by whichever stage ran; see @ref dither for what
///        the count means.
Audio output_chain(const Audio& audio, const OutputChainConfig& config = {},
                   size_t* non_finite_samples = nullptr);

}  // namespace sonare::mastering::final
