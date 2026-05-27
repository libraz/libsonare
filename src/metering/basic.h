#pragma once

/// @file basic.h
/// @brief Basic offline audio meters.

#include "core/audio.h"

namespace sonare::metering {

float peak_db(const Audio& audio);
float rms_db(const Audio& audio);
float crest_factor_db(const Audio& audio);
float clipping_ratio(const Audio& audio, float threshold = 0.999f);
float silence_ratio(const Audio& audio, float threshold_db = -45.0f, int frame_length = 1024,
                    int hop_length = 256);
float dc_offset(const Audio& audio);

}  // namespace sonare::metering
