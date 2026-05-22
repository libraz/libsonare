#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

enum class TrimSilenceMode {
  Peak,
  LufsGated,
};

struct TrimSilenceConfig {
  float threshold = 0.001f;
  size_t padding_samples = 0;
  TrimSilenceMode mode = TrimSilenceMode::Peak;
  float gate_lufs = -60.0f;
  float window_ms = 400.0f;
};

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config = {});

}  // namespace sonare::mastering::repair
