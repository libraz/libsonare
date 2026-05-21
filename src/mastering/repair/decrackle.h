#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DecrackleConfig {
  float threshold = 0.4f;
};

Audio decrackle(const Audio& audio, const DecrackleConfig& config = {});

}  // namespace sonare::mastering::repair
