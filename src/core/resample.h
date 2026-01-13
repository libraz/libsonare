#pragma once

/// @file resample.h
/// @brief High-quality audio resampling using r8brain.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Resamples audio to a target sample rate.
/// @param audio Input audio
/// @param target_sr Target sample rate in Hz
/// @return Resampled audio at target sample rate
Audio resample(const Audio& audio, int target_sr);

/// @brief Resamples raw samples to a target sample rate.
/// @param samples Input samples
/// @param src_sr Source sample rate in Hz
/// @param target_sr Target sample rate in Hz
/// @return Resampled samples
std::vector<float> resample(const float* samples, size_t size, int src_sr, int target_sr);

}  // namespace sonare
