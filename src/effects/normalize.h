#pragma once

/// @file normalize.h
/// @brief Audio normalization and silence trimming.

#include "core/audio.h"

namespace sonare {

/// @brief Normalizes audio to a target peak level.
/// @param audio Input audio
/// @param target_db Target peak level in dB (default 0 dB = full scale)
/// @return Normalized audio
Audio normalize(const Audio& audio, float target_db = 0.0f);

/// @brief Normalizes audio using RMS level.
/// @param audio Input audio
/// @param target_db Target RMS level in dB
/// @return Normalized audio
Audio normalize_rms(const Audio& audio, float target_db = -20.0f);

/// @brief Trims silence from the beginning and end of audio.
/// @param audio Input audio
/// @param threshold_db Silence threshold in dB (samples below this are considered silence)
/// @param frame_length Frame length for silence detection in samples
/// @param hop_length Hop length for silence detection in samples
/// @return Trimmed audio
Audio trim(const Audio& audio, float threshold_db = -60.0f, int frame_length = 2048,
           int hop_length = 512);

/// @brief Finds the start and end indices of non-silent audio.
/// @param audio Input audio
/// @param threshold_db Silence threshold in dB
/// @param frame_length Frame length for silence detection
/// @param hop_length Hop length for silence detection
/// @return Pair of (start_sample, end_sample) indices
std::pair<size_t, size_t> detect_silence_boundaries(const Audio& audio, float threshold_db = -60.0f,
                                                    int frame_length = 2048, int hop_length = 512);

/// @brief Computes peak amplitude in dB.
/// @param audio Input audio
/// @return Peak amplitude in dB (0 dB = full scale)
float peak_db(const Audio& audio);

/// @brief Computes RMS level in dB.
/// @param audio Input audio
/// @return RMS level in dB
float rms_db(const Audio& audio);

/// @brief Applies gain to audio.
/// @param audio Input audio
/// @param gain_db Gain in dB (positive = louder, negative = quieter)
/// @return Audio with applied gain
Audio apply_gain(const Audio& audio, float gain_db);

/// @brief Applies fade in to audio.
/// @param audio Input audio
/// @param duration_sec Fade duration in seconds
/// @return Audio with fade in applied
Audio fade_in(const Audio& audio, float duration_sec);

/// @brief Applies fade out to audio.
/// @param audio Input audio
/// @param duration_sec Fade duration in seconds
/// @return Audio with fade out applied
Audio fade_out(const Audio& audio, float duration_sec);

}  // namespace sonare
