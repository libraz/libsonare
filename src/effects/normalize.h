#pragma once

/// @file normalize.h
/// @brief Audio normalization and silence trimming.

#include "core/audio.h"
#include "effects/silence.h"

namespace sonare {

/// Peak level of normalize() when a caller gives none (full scale).
inline constexpr float kDefaultPeakTargetDb = 0.0f;
/// RMS level of normalize_rms() when a caller gives none.
inline constexpr float kDefaultRmsTargetDb = -20.0f;
/// Absolute dBFS level under which trim_absolute() reads a frame as silence when a caller gives
/// none.
inline constexpr float kDefaultAbsoluteSilenceDb = -60.0f;

/// @brief Normalizes audio to a target peak level.
/// @details One gain via @ref apply_gain. The result is not clipped; a caller writing to an
///          integer format fits or limits it. A target above 0 dB leaves the peak above full scale.
/// @param audio Input audio
/// @param target_db Target peak level in dB, finite (default 0 dB = full scale)
/// @return Normalized audio; an empty input comes back empty.
/// @throws SonareException with ErrorCode::InvalidParameter when the audio is digital silence:
///         it has no measurable level to scale from.
Audio normalize(const Audio& audio, float target_db = kDefaultPeakTargetDb);

/// @brief Normalizes audio using RMS level.
/// @details One gain brings the signal RMS exactly to @p target_db. The result is not clipped;
///          a caller writing to an integer format fits or limits it. An RMS target can leave
///          peaks above full scale.
/// @param audio Input audio
/// @param target_db Target RMS level in dB, finite
/// @return Normalized audio; an empty input comes back empty.
/// @throws SonareException with ErrorCode::InvalidParameter for digital silence, as
///         @ref normalize does.
Audio normalize_rms(const Audio& audio, float target_db = kDefaultRmsTargetDb);

/// @brief A normalized channel pair and the gain that produced it.
struct NormalizeStereoResult {
  Audio left;
  Audio right;
  /// One figure rather than one per channel: the level is measured across both
  /// channels and the gain is applied to both, so a per-channel pair would be
  /// two copies of one decision and would read as though the two could differ.
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
///   arguments and the silent input @ref normalize rejects.
NormalizeStereoResult normalize_stereo(const Audio& left, const Audio& right,
                                       float target_db = kDefaultPeakTargetDb);

/// @brief Two-channel @ref normalize_rms.
/// @details The RMS is taken over both channels together — the root mean square
///   of the pair, not the mean of two separate figures — and the resulting gain
///   goes to both, for the same reason @ref normalize_stereo shares its gain.
/// @throws SonareException as @ref normalize_stereo does.
NormalizeStereoResult normalize_rms_stereo(const Audio& left, const Audio& right,
                                           float target_db = kDefaultRmsTargetDb);

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
Audio trim_absolute(const Audio& audio, float threshold_db = kDefaultAbsoluteSilenceDb,
                    int frame_length = kDefaultSilenceFrameLength,
                    int hop_length = kDefaultSilenceHopLength);

/// @brief Finds the start and end indices of non-silent audio.
/// @param audio Input audio
/// @param threshold_db Silence threshold in dB
/// @param frame_length Frame length for silence detection
/// @param hop_length Hop length for silence detection
/// @return Pair of (start_sample, end_sample) indices
std::pair<size_t, size_t> detect_silence_boundaries(const Audio& audio,
                                                    float threshold_db = kDefaultAbsoluteSilenceDb,
                                                    int frame_length = kDefaultSilenceFrameLength,
                                                    int hop_length = kDefaultSilenceHopLength);

/// @brief Applies gain to audio.
/// @details The result is not clipped; a gain past full scale leaves samples above 1.
/// @param audio Input audio
/// @param gain_db Gain in dB (positive = louder, negative = quieter)
/// @return Audio with applied gain
/// @throws SonareException(InvalidParameter) when @p gain_db is not finite or has
///         no finite linear multiplier.
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
