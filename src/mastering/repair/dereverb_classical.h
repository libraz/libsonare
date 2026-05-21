#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DereverbClassicalConfig {
  float threshold = 0.05f;
  float attenuation = 0.5f;
};

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
