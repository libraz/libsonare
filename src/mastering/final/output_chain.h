#pragma once

#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"

namespace sonare::mastering::final {

struct OutputChainConfig {
  int target_bits = 16;
  DitherType dither_type = DitherType::Tpdf;
  bool clamp = true;
};

Audio output_chain(const Audio& audio, const OutputChainConfig& config = {});

}  // namespace sonare::mastering::final
