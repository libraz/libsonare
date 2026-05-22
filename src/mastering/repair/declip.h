#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DeclipConfig {
  float clip_threshold = 0.98f;
  int lpc_order = 36;
  int iterations = 2;
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

}  // namespace sonare::mastering::repair
