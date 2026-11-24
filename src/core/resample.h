#pragma once

/// @file resample.h
/// @brief High-quality audio resampling using r8brain.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Resamples audio to a target sample rate.
/// @param audio Input audio
/// @param target_sample_rate Positive target sample rate in Hz for an already-admitted stream.
/// @return Resampled audio at target sample rate. A call already at @p target_sample_rate
///         returns a view sharing @p audio's buffer rather than a duplicate, so
///         it costs no allocation and keeps that buffer alive for as long as the
///         result lives. The buffer is immutable, so sharing it is unobservable
///         apart from that lifetime.
/// @throws SonareException with ErrorCode::InvalidParameter for an unsupported target rate.
Audio resample(const Audio& audio, int target_sample_rate);

/// @brief Resamples raw samples to a target sample rate.
/// @details Internal engine; untrusted input goes through resample_checked.
/// @param samples Input samples
/// @param src_sr Source sample rate in Hz
/// @param target_sr Target sample rate in Hz
/// @return Resampled samples
std::vector<float> resample(const float* samples, size_t size, int src_sr, int target_sr);

/// @brief Resamples raw samples after validating them as untrusted input.
/// @details The entry for caller-supplied input (C ABI, Node addon, WASM embind). Both rates must
///          lie within [kMinAudioSampleRate, kMaxAudioSampleRate], the samples must be finite, and
///          the projected output length must not exceed kMaxAudioBufferSize. An empty input
///          returns an empty vector once the rates are accepted.
/// @param samples Input samples
/// @param size Number of input samples
/// @param src_sr Source sample rate in Hz
/// @param target_sr Target sample rate in Hz
/// @return Resampled samples
/// @throws SonareException with ErrorCode::InvalidParameter on any refused argument
std::vector<float> resample_checked(const float* samples, size_t size, int src_sr, int target_sr);

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
