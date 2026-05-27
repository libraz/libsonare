#pragma once

/// @file native_spectral_stretch.h
/// @brief Offline spectral time/pitch stretch implemented with libsonare primitives.

#include "core/audio.h"

namespace sonare {

Audio native_spectral_time_stretch(const Audio& audio, float rate);
Audio native_spectral_pitch_shift_ratio(const Audio& audio, float ratio);

}  // namespace sonare
