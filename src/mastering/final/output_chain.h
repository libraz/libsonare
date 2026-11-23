#pragma once

#include <cstddef>

#include "core/quantize_report.h"
#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"

namespace sonare::mastering::final {

struct OutputChainConfig {
  int target_bits = 16;
  DitherType dither_type = DitherType::Tpdf;
};

/// @brief Quantizes @p audio to @c config.target_bits through exactly one stage.
/// @details @c DitherType::None runs @ref bit_depth; any other type runs @ref dither alone, whose
///          output already sits on the target grid inside the code range.
/// @param[out] report Optional counts as reported by whichever stage ran; see
///        @ref QuantizeReport for what they mean.
Audio output_chain(const Audio& audio, const OutputChainConfig& config = {},
                   QuantizeReport* report = nullptr);

}  // namespace sonare::mastering::final
