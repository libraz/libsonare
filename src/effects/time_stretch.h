#pragma once

/// @file time_stretch.h
/// @brief Time stretching without pitch change.

#include "core/audio.h"
#include "core/spectrum.h"

namespace sonare {

/// @brief Configuration for time stretching.
struct TimeStretchConfig {
  int n_fft = 2048;      ///< FFT size for analysis
  int hop_length = 512;  ///< Hop length for analysis
};

/// @brief Time-stretches audio without changing pitch.
/// @details Uses phase vocoder internally.
/// @param audio Input audio
/// @param rate Time stretch rate (< 1.0 = slower/longer, > 1.0 = faster/shorter)
/// @param config Time stretch configuration
/// @return Time-stretched audio
/// @note rate = 0.5 doubles the duration, rate = 2.0 halves it
Audio time_stretch(const Audio& audio, float rate,
                   const TimeStretchConfig& config = TimeStretchConfig());

}  // namespace sonare
