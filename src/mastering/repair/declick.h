#pragma once

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DeclickConfig {
  float threshold = 0.8f;
  float neighbor_ratio = 4.0f;
  size_t max_click_samples = 8;
  int lpc_order = 20;
  float residual_ratio = 8.0f;
};

Audio declick(const Audio& audio, const DeclickConfig& config = {});

}  // namespace sonare::mastering::repair
