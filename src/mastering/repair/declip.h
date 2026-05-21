#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DeclipConfig {
  float clip_threshold = 0.98f;
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

}  // namespace sonare::mastering::repair
