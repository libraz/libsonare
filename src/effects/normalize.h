#pragma once

/// @file normalize.h
/// @brief Audio normalization and silence trimming.

#include "core/audio.h"

namespace sonare {

/// @brief Normalizes audio to a target peak level.
/// @details Internally applies a gain via @ref apply_gain. With the default
///          target of 0 dB this is at full scale. Positive targets are rejected
///          when @p clip is true; pass @p clip = false only when downstream
///          headroom management is intentional.
/// @param audio Input audio
/// @param target_db Target peak level in dB (default 0 dB = full scale)
/// @param clip If true (default), the output is hard-clipped to [-1, 1]; pass
///        false to leave the signal unclipped.
/// @return Normalized audio
Audio normalize(const Audio& audio, float target_db = 0.0f, bool clip = true);

/// @brief Normalizes audio using RMS level.
/// @details Applies a uniform gain to bring the signal RMS to @p target_db. Note
///          that because peaks sit well above the RMS level, RMS normalisation
///          to a loud target routinely drives samples past full scale. With the
///          default @p clip = true those overshoots are hard-clipped to [-1, 1]
///          (introducing audible distortion); pass @p clip = false to preserve
///          the unclipped (possibly >1) signal and manage headroom downstream.
/// @param audio Input audio
/// @param target_db Target RMS level in dB
/// @param clip If true (default), hard-clip the output to [-1, 1]
/// @return Normalized audio
Audio normalize_rms(const Audio& audio, float target_db = -20.0f, bool clip = true);

/// @brief A normalized channel pair and the gain that produced it.
struct NormalizeStereoResult {
  Audio left;
  Audio right;
  /// One figure rather than one per channel: the level is measured across both
  /// channels and the gain is applied to both, so a per-channel pair would be
  /// two copies of one decision and would read as though the two could differ.
  /// Silence leaves the pair untouched and reports 0.
  float applied_gain_db;
};

/// @brief Two-channel @ref normalize.
/// @details The peak is taken across both channels and the resulting gain goes
///   to both, because a per-channel gain is a balance change: it would move the
///   quieter side up until the two peaks matched, which is not what normalizing
///   a stereo recording means. The louder channel therefore reaches @p target_db
///   and the other keeps its level relative to it.
/// @throws SonareException with ErrorCode::InvalidParameter for a pair that is
///   not processable together (see @ref require_stereo_pair), and for the
///   arguments @ref normalize rejects.
NormalizeStereoResult normalize_stereo(const Audio& left, const Audio& right,
                                       float target_db = 0.0f, bool clip = true);

/// @brief Two-channel @ref normalize_rms.
/// @details The RMS is taken over both channels together — the root mean square
///   of the pair, not the mean of two separate figures — and the resulting gain
///   goes to both, for the same reason @ref normalize_stereo shares its gain.
/// @throws SonareException as @ref normalize_stereo does.
NormalizeStereoResult normalize_rms_stereo(const Audio& left, const Audio& right,
                                           float target_db = -20.0f, bool clip = true);

/// @brief Trims silence from the beginning and end of audio using an ABSOLUTE
///        RMS threshold.
/// @details Frames whose RMS is below @p threshold_db (an absolute dBFS level)
///          are treated as silence. This is NOT librosa-compatible — for the
///          librosa-style relative-to-peak behavior use sonare::trim(const
///          float*, ...) / sonare::trim(const std::vector<float>&, ...) in
///          src/effects/silence.h, which interprets its threshold as `top_db`
///          BELOW the peak RMS (note the opposite default sign: +60 vs -60 dB).
/// @param audio Input audio
/// @param threshold_db Absolute silence threshold in dBFS (frames below this
///        are considered silence)
/// @param frame_length Frame length for silence detection in samples
/// @param hop_length Hop length for silence detection in samples
/// @return Trimmed audio
Audio trim_absolute(const Audio& audio, float threshold_db = -60.0f, int frame_length = 2048,
                    int hop_length = 512);

/// @brief Finds the start and end indices of non-silent audio.
/// @param audio Input audio
/// @param threshold_db Silence threshold in dB
/// @param frame_length Frame length for silence detection
/// @param hop_length Hop length for silence detection
/// @return Pair of (start_sample, end_sample) indices
std::pair<size_t, size_t> detect_silence_boundaries(const Audio& audio, float threshold_db = -60.0f,
                                                    int frame_length = 2048, int hop_length = 512);

/// @brief Applies gain to audio.
///
/// When @p clip is true (the default, preserving historical behavior) output
/// samples are hard-clipped to the [-1, 1] range after gain is applied, so the
/// result is silently saturated when the gain would exceed full scale. Pass
/// @p clip = false to leave the gained signal unclipped (e.g. when the caller
/// applies its own limiter/headroom management downstream).
///
/// @param audio Input audio
/// @param gain_db Gain in dB (positive = louder, negative = quieter)
/// @param clip If true, hard-clip the output to [-1, 1]; if false, no clipping
/// @return Audio with applied gain (clipped to [-1, 1] when @p clip is true)
Audio apply_gain(const Audio& audio, float gain_db, bool clip = true);

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
