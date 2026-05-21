#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct TrimSilenceConfig {
  float threshold = 0.001f;
  size_t padding_samples = 0;
};

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config = {});

}  // namespace sonare::mastering::repair
