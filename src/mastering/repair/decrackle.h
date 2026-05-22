#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

enum class DecrackleMode {
  Median,
  WaveletShrinkage,
};

struct DecrackleConfig {
  float threshold = 0.4f;
  DecrackleMode mode = DecrackleMode::Median;
  int levels = 4;
};

Audio decrackle(const Audio& audio, const DecrackleConfig& config = {});

}  // namespace sonare::mastering::repair
