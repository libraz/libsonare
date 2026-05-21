#pragma once

#include "core/audio.h"

namespace sonare::mastering::final {

struct BitDepthConfig {
  int target_bits = 16;
  bool clamp = true;
};

Audio bit_depth(const Audio& audio, const BitDepthConfig& config = {});

}  // namespace sonare::mastering::final
