#pragma once

/// @file native_spectral_stretch.h
/// @brief Offline spectral time/pitch stretch implemented with libsonare primitives.

#include "core/audio.h"
#include "util/constants.h"

namespace sonare {

/// @brief Offline spectral time stretch on the native libsonare path.
/// @param n_fft FFT size for analysis. Non-positive values fall back to the
///        default (@c constants::kDefaultNFft).
/// @param hop_length Hop length for analysis. Non-positive values fall back to
///        the default (@c constants::kDefaultHopLength).
Audio native_spectral_time_stretch(const Audio& audio, float rate,
                                   int n_fft = constants::kDefaultNFft,
                                   int hop_length = constants::kDefaultHopLength);
/// @brief Offline spectral pitch shift on the native libsonare path.
/// @param n_fft FFT size for the internal time stretch (see above; non-positive
///        falls back to the default).
/// @param hop_length Hop length for the internal time stretch (non-positive
///        falls back to the default).
Audio native_spectral_pitch_shift_ratio(const Audio& audio, float ratio,
                                        int n_fft = constants::kDefaultNFft,
                                        int hop_length = constants::kDefaultHopLength);

}  // namespace sonare
