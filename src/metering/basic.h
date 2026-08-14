#pragma once

/// @file basic.h
/// @brief Basic offline audio meters.

#include <cstddef>

#include "core/audio.h"

namespace sonare::metering {

float peak_db(const Audio& audio);
float rms_db(const Audio& audio);
float crest_factor_db(const Audio& audio);

/// @brief Crest factor (peak minus RMS, in dB) of an interleaved buffer.
/// @details Takes the peak as the largest absolute sample across all channels
///          and the RMS over every sample of every channel, so the measurement
///          survives material a `0.5 * (L + R)` downmix would collapse: an
///          out-of-phase pair cancels in the downmix, which understates its RMS
///          and overstates the crest factor.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @return Peak-to-RMS ratio in dB, or 0 for silent or empty input, matching
///         the mono `crest_factor_db` convention.
float crest_factor_db_interleaved(const float* samples, std::size_t frames, int channels);
float clipping_ratio(const Audio& audio, float threshold = 0.999f);
float silence_ratio(const Audio& audio, float threshold_db = -45.0f, int frame_length = 1024,
                    int hop_length = 256);
float dc_offset(const Audio& audio);

}  // namespace sonare::metering
