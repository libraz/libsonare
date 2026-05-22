#pragma once

/// @file onset.h
/// @brief Onset strength computation for rhythm analysis.

#include <vector>

#include "feature/mel_spectrogram.h"

namespace sonare {

/// @brief Configuration for onset strength computation.
struct OnsetConfig {
  int lag = 1;          ///< Time lag for computing differences (frames)
  bool detrend = true;  ///< Remove DC component from onset curve
  bool center = true;   ///< Pad frames so each onset value is aligned to the frame center
};

/// @brief Computes onset strength envelope from Mel spectrogram.
/// @details Uses half-wave rectified first-order difference of log Mel spectrogram.
/// @param mel_spec Mel spectrogram
/// @param config Onset configuration
/// @return Onset strength envelope [n_frames]
std::vector<float> compute_onset_strength(const MelSpectrogram& mel_spec,
                                          const OnsetConfig& config = OnsetConfig());

/// @brief Computes onset strength envelope from audio.
/// @param audio Input audio
/// @param mel_config Mel spectrogram configuration
/// @param onset_config Onset configuration
/// @return Onset strength envelope [n_frames]
std::vector<float> compute_onset_strength(const Audio& audio,
                                          const MelConfig& mel_config = MelConfig(),
                                          const OnsetConfig& onset_config = OnsetConfig());

/// @brief Computes multi-band onset strength (separate strength for different frequency bands).
/// @param mel_spec Mel spectrogram
/// @param n_bands Number of frequency bands (divides Mel bands evenly)
/// @param config Onset configuration
/// @return Multi-band onset strength [n_bands x n_frames]
std::vector<float> onset_strength_multi(const MelSpectrogram& mel_spec, int n_bands = 3,
                                        const OnsetConfig& config = OnsetConfig());

/// @brief Backtracks each onset event to the most recent local minimum of an
///        energy-like signal (librosa.onset.onset_backtrack).
/// @param events Indices of detected onsets (frame indices)
/// @param energy Reference energy curve aligned with @p events (typically the
///        onset envelope or short-time energy)
/// @return Adjusted onset indices, each shifted to the nearest preceding local
///         minimum of @p energy. Returned values are clipped to [0, energy.size()).
std::vector<int> onset_backtrack(const std::vector<int>& events, const std::vector<float>& energy);

/// @brief Computes spectral flux (unsigned L1 norm of spectral difference).
/// @param spec Spectrogram
/// @param lag Time lag for computing differences
/// @return Spectral flux envelope [n_frames]
std::vector<float> spectral_flux(const Spectrogram& spec, int lag = 1);

}  // namespace sonare
