#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DeclickConfig {
  float threshold = 0.8f;
  float neighbor_ratio = 4.0f;
  size_t max_click_samples = 8;
};

Audio declick(const Audio& audio, const DeclickConfig& config = {});

}  // namespace sonare::mastering::repair
