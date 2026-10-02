#pragma once

/// @file resample.h
/// @brief High-quality audio resampling using r8brain.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Resamples audio to a target sample rate.
/// @param audio Input audio
/// @param target_sr Target sample rate in Hz
/// @return Resampled audio at target sample rate. A call already at @p target_sr
///         returns a view sharing @p audio's buffer rather than a duplicate, so
///         it costs no allocation and keeps that buffer alive for as long as the
///         result lives. The buffer is immutable, so sharing it is unobservable
///         apart from that lifetime.
Audio resample(const Audio& audio, int target_sr);

/// @brief Resamples raw samples to a target sample rate.
/// @param samples Input samples
/// @param src_sr Source sample rate in Hz
/// @param target_sr Target sample rate in Hz
/// @return Resampled samples
std::vector<float> resample(const float* samples, size_t size, int src_sr, int target_sr);

/// @brief Resamples a finite impulse response while preserving its DC gain.
/// @param samples Input FIR coefficients.
/// @param size Number of input coefficients.
/// @param src_sr Source sample rate in Hz.
/// @param target_sr Target sample rate in Hz.
/// @return Coefficients scaled so their sum matches the input's; when either sum is near zero
///         relative to its L1 norm, the signs differ, or the measured gain lies outside
///         [0.5, 2] x src_sr / target_sr, scaled by src_sr / target_sr instead.
std::vector<float> resample_impulse_response(const float* samples, size_t size, int src_sr,
                                             int target_sr);

}  // namespace sonare
