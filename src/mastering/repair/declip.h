#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DeclipConfig {
  float clip_threshold = 0.98f;
  int lpc_order = 36;
  int iterations = 2;
  // Blend weight for the LPC prediction; the interpolation fallback gets (1 - lpc_blend).
  float lpc_blend = 0.65f;
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

}  // namespace sonare::mastering::repair
