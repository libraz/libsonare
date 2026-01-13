#pragma once

/// @file pitch_shift.h
/// @brief Pitch shifting without changing duration.

#include "core/audio.h"

namespace sonare {

/// @brief Configuration for pitch shifting.
struct PitchShiftConfig {
  int n_fft = 2048;      ///< FFT size for time stretch
  int hop_length = 512;  ///< Hop length for time stretch
};

/// @brief Pitch-shifts audio without changing duration.
/// @details Uses time stretching followed by resampling.
/// @param audio Input audio
/// @param semitones Pitch shift in semitones (positive = higher, negative = lower)
/// @param config Pitch shift configuration
/// @return Pitch-shifted audio with same duration
/// @note +12 semitones = one octave higher, -12 = one octave lower
Audio pitch_shift(const Audio& audio, float semitones,
                  const PitchShiftConfig& config = PitchShiftConfig());

/// @brief Pitch-shifts audio by a frequency ratio.
/// @param audio Input audio
/// @param ratio Frequency ratio (2.0 = one octave higher, 0.5 = one octave lower)
/// @param config Pitch shift configuration
/// @return Pitch-shifted audio with same duration
Audio pitch_shift_ratio(const Audio& audio, float ratio,
                        const PitchShiftConfig& config = PitchShiftConfig());

}  // namespace sonare
