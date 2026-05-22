#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DehumConfig {
  float fundamental_hz = 50.0f;
  int harmonics = 4;
  float q = 20.0f;
  bool adaptive = false;
  float search_range_hz = 2.0f;
  float adaptation = 0.25f;
  int frame_size = 2048;
  float pll_bandwidth = 0.01f;
};

Audio dehum(const Audio& audio, const DehumConfig& config = {});

}  // namespace sonare::mastering::repair
