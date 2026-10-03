#pragma once

/// @file ab_switcher.h
/// @brief A/B audition helper with equal-length crossfade and loudness matching.

#include "core/audio.h"

namespace sonare::mastering::match {

enum class ABSelection { A, B };

void validate_selection(ABSelection selection);
Audio ab_switch(const Audio& a, const Audio& b, ABSelection selection);
Audio ab_crossfade(const Audio& a, const Audio& b, float mix);

/// @brief Result of matching `b`'s BS.1770 integrated loudness to `a`'s.
struct LoudnessMatchedPair {
  Audio a;  ///< Returned unchanged; the loudness reference.
  Audio b;  ///< `b` with a gain applied to match `a`'s integrated LUFS.
  /// `a`'s integrated loudness. Non-finite when `a` is silent or below the
  /// absolute gate, which is also when `applied_gain_db` falls back to 0.
  float reference_lufs = 0.0f;
  float source_lufs = 0.0f;             ///< `b`'s, before the match. Same non-finite case.
  float applied_gain_db = 0.0f;         ///< Gain applied to `b`, in dB.
  float matched_true_peak_dbtp = 0.0f;  ///< `b`'s true peak after the gain, in dBTP.
};

/// @brief One planar stereo pair used by the A/B helpers.
struct StereoAudioPair {
  Audio left;
  Audio right;
};

/// @brief Crossfade two planar stereo pairs with one linear mix value.
/// @details Each pair must have equal per-plane lengths and sample rates; the
///          two pairs may have different lengths. The result follows the
///          shorter pair and leaves both inputs untouched.
StereoAudioPair ab_crossfade_stereo(const StereoAudioPair& a, const StereoAudioPair& b, float mix);

/// @brief Stereo counterpart of @ref LoudnessMatchedPair.
/// @details `a` is the loudness reference and `b` is the source after one
///          gain shared by both channels. The gain is deliberately uncapped;
///          the true peak reports the result a caller must inspect.
struct StereoLoudnessMatchedPair {
  StereoAudioPair a;
  StereoAudioPair b;
  float reference_lufs = 0.0f;
  float source_lufs = 0.0f;
  float applied_gain_db = 0.0f;
  float matched_true_peak_dbtp = 0.0f;
};

/// @brief Gain-matches `b` to `a`'s BS.1770 integrated loudness, so feeding
///        the result into @ref ab_switch or @ref ab_crossfade compares the
///        two without a loudness bias.
/// @details Applies `measure_lufs(a) - measure_lufs(b)` to `b` with no upper
///          bound: this API only reports the post-gain true peak rather than
///          capping it, because a bare headroom clamp would leave `b` at its
///          own loudness whenever it started near full scale, defeating the
///          only reason this function exists. Returns 0 dB unchanged when
///          either integrated loudness is non-finite (silence, or below the
///          absolute gate).
LoudnessMatchedPair ab_match_loudness(const Audio& a, const Audio& b);

/// @brief Gain-match a stereo source pair to a stereo reference pair.
/// @details The LUFS measurement uses BS.1770 channel summing, so antiphase or
///          one-sided material remains measurable. One gain is applied to both
///          planes, preserving their ratio and phase. The pair must have equal
///          per-pair lengths and sample rates; each pair may have a different
///          length from the other.
StereoLoudnessMatchedPair ab_match_loudness_stereo(const StereoAudioPair& a,
                                                   const StereoAudioPair& b);

}  // namespace sonare::mastering::match
