#pragma once

/// @file phase_vocoder.h
/// @brief Phase vocoder for time-scale modification.

#include <vector>

#include "core/spectrum.h"

namespace sonare {

/// @brief Configuration for phase vocoder.
struct PhaseVocoderConfig {
  int hop_length = 512;  ///< Hop length for analysis/synthesis
};

/// @brief Performs phase vocoder time-stretching on a spectrogram.
/// @details Resamples the spectrogram in time while maintaining phase coherence.
/// @param spec Input spectrogram
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster)
/// @param config Phase vocoder configuration
/// @return Time-stretched spectrogram
Spectrogram phase_vocoder(const Spectrogram& spec, float rate,
                          const PhaseVocoderConfig& config = PhaseVocoderConfig());

/// @brief Computes instantaneous frequency from phase difference.
/// @param phase Current phase values [n_bins]
/// @param prev_phase Previous phase values [n_bins]
/// @param n_bins Number of frequency bins
/// @param hop_length Hop length in samples
/// @param sample_rate Sample rate in Hz
/// @return Instantaneous frequency in Hz [n_bins]
std::vector<float> compute_instantaneous_frequency(const float* phase, const float* prev_phase,
                                                   int n_bins, int hop_length, int sample_rate);

}  // namespace sonare
