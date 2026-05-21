#pragma once

#include <cstdint>

#include "core/audio.h"

namespace sonare::mastering::final {

enum class DitherType { None, Rpdf, Tpdf, NoiseShaped };

struct DitherConfig {
  DitherType type = DitherType::Tpdf;
  int target_bits = 16;
  uint32_t seed = 0x51A7E5u;
};

Audio dither(const Audio& audio, const DitherConfig& config = {});

}  // namespace sonare::mastering::final
