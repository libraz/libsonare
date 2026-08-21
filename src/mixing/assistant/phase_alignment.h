#pragma once

/// @file phase_alignment.h
/// @brief Time and polarity relationship between every pair of tracks.
///
/// @details **Offline / control thread only.** The pass is quadratic in track
///          count, allocates a mono excerpt per pair and runs a direct
///          cross-correlation over it. Never call any of this from `process()`.
///
/// @details The measurement is a single normalized cross-correlation peak
///          search per pair. There is no coarse stage: the excerpt is short
///          enough and the lag range narrow enough that the direct search
///          already resolves the offset to the sample, and a preliminary pass
///          could only ever move the answer away from that.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Default half-width of the lag search, in milliseconds.
/// @details Read back from the speed of sound rather than picked by ear. Sound
///          covers roughly 34 cm per millisecond in air, so two microphones 5 m
///          apart on the same source differ by about 15 ms of arrival time, and
///          30 ms covers a path difference of about 10 m — further apart than a
///          close mic and a room mic in any normal tracking room. Searching
///          wider costs time in proportion and starts matching a periodic
///          signal's neighbouring cycle instead of its true arrival offset.
inline constexpr float kDefaultMaxLagMs = 30.0f;

/// @brief Hard ceiling on the searched half-width, in milliseconds.
/// @details 100 ms is a path difference of about 34 m. An offset larger than
///          that is an edit or an arrangement decision, not two microphones on
///          one source, and searching for it would cost time proportionally
///          while inviting spurious matches.
inline constexpr float kMaxSearchLagMs = 100.0f;

/// @brief Default `|r|` below which a pair is reported as unrelated.
/// @details Two tracks that share less than half their energy shape cannot be
///          given a common arrival time with any confidence, and delaying or
///          flipping one of them on that evidence does audible harm. Bleed-only
///          relationships land below this and are deliberately left alone.
inline constexpr float kDefaultMinAbsCorrelation = 0.5f;

/// @brief Default length of the excerpt each pair is measured over, in seconds.
/// @details The direct search costs `(2 * lag_range + 1) * window_samples`
///          multiply-adds per pair and the pair count is quadratic, so the
///          window is the main cost lever. One second of the passage where both
///          tracks are most active is far more than a time offset needs, and it
///          keeps a full mix's worth of pairs to a few seconds of work.
inline constexpr float kDefaultAnalysisWindowSec = 1.0f;

/// @brief Shortest excerpt the window length is allowed to be clamped to.
inline constexpr float kMinAnalysisWindowSec = 0.05f;

/// @brief Longest excerpt the window length is allowed to be clamped to.
/// @details A cost ceiling, not a quality one: beyond a few seconds the peak
///          stops moving while the search keeps getting more expensive.
inline constexpr float kMaxAnalysisWindowSec = 5.0f;

/// @brief Tunables for @ref analyze_phase_alignment.
/// @details Out-of-range values are clamped rather than rejected: this is an
///          analysis pass with no error channel, and a caller who asks for a
///          400 ms search wants the widest search available, not an exception.
struct PhaseAlignmentConfig {
  /// @brief Search half-width, derived from the speed of sound.
  /// @details Clamped to `[0, kMaxSearchLagMs]`. Zero measures polarity only.
  float max_lag_ms = kDefaultMaxLagMs;
  /// @brief Below this the pair is left alone. Clamped to `[0, 1]`.
  float min_abs_correlation = kDefaultMinAbsCorrelation;
  /// @brief Length of the shared excerpt evaluated, in seconds.
  /// @details Clamped to `[kMinAnalysisWindowSec, kMaxAnalysisWindowSec]`. A
  ///          track shorter than the window is measured over its real length.
  float analysis_window_sec = kDefaultAnalysisWindowSec;
};

/// @brief Measures the time and polarity relationship of every track pair.
/// @details One entry per unordered pair, reference index first, in ascending
///          `(reference, target)` order. Stereo tracks are evaluated as their
///          mono sum; the correlation is normalized, so a level difference
///          between two tracks cannot bias the result the way a raw
///          cross-correlation does.
///
///          Both tracks of a pair are excerpted at the **same time position** —
///          the window where the two are jointly most active — because a lag
///          measured between two different passages means nothing. A pair whose
///          tracks were recorded at different sample rates is left unmeasured
///          for the same reason: a lag in samples has no shared meaning across
///          two rates.
///
///          Every field is the measurement as taken: @ref
///          PairAlignment::lag_samples and @ref PairAlignment::polarity_opposed
///          describe the strongest `|r|` peak found, and @ref
///          PairAlignment::related alone says whether acting on it is
///          warranted. An unmeasurable pair — either track unusable, silent
///          over the excerpt, too short, or non-finite — is returned as an
///          entry with the two indices filled and everything else at its
///          default, so the matrix is always complete and never carries a NaN.
/// @param tracks Tracks to compare, planar, mono or stereo.
/// @param profiles Per-track profiles from @ref analyze_track_profiles, in the
///        same order. A missing or unusable profile makes every pair containing
///        that track unrelated.
/// @param config Search and gating tunables.
/// @return `n * (n - 1) / 2` entries; empty for fewer than two tracks.
std::vector<PairAlignment> analyze_phase_alignment(const std::vector<TrackInput>& tracks,
                                                   const std::vector<TrackProfile>& profiles,
                                                   const PhaseAlignmentConfig& config = {});

}  // namespace sonare::mixing::assistant
