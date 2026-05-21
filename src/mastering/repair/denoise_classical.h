#pragma once

#include "core/audio.h"

namespace sonare::mastering::repair {

struct DenoiseClassicalConfig {
  float noise_floor = 0.02f;
  float attenuation = 0.25f;
};

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config = {});

}  // namespace sonare::mastering::repair
