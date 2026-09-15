#pragma once

#include <cstddef>

#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"

namespace sonare::mastering::final {

struct OutputChainConfig {
  int target_bits = 16;
  DitherType dither_type = DitherType::Tpdf;
  bool clamp = true;
};

/// @brief Runs the dither and bit-depth stages in that order.
/// @param[out] non_finite_samples Optional count of input samples that were not
///        finite, summed over both stages. The dither stage runs first and
///        substitutes every one of them, so the bit-depth stage contributes
///        nothing in practice; see @ref dither for what the count means.
Audio output_chain(const Audio& audio, const OutputChainConfig& config = {},
                   size_t* non_finite_samples = nullptr);

}  // namespace sonare::mastering::final
