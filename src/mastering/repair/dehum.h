#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DehumConfig {
  float fundamental_hz = 50.0f;
  int harmonics = 4;
  float q = 20.0f;
};

Audio dehum(const Audio& audio, const DehumConfig& config = {});

}  // namespace sonare::mastering::repair
