#pragma once

/// @file phase_alignment.h
/// @brief Time and polarity relationship between every pair of tracks.
///
/// @details **Offline / control thread only.** The pass is quadratic in track
///          count and allocates a mono excerpt per pair.
///
/// @details One normalized cross-correlation peak search per pair, with no
///          coarse stage — every lag is evaluated at full sample resolution. The
///          lag range goes through one transform pair rather than a pass per
///          lag, which computes the same sums a different way, and the winning
///          lag's correlation is then recomputed exactly in double, so the
///          transform decides only which lag wins.
///
/// @details Cost is about 0.8 ms per pair on 2 s mono at 48 kHz, so a 24-track
///          session's 276 pairs take roughly 0.2 s. Budget against the pair
///          count, which is quadratic; the per-pair cost is bounded by
///          @ref PhaseAlignmentConfig::analysis_window_sec and the sample rate.
///
/// @details The image domain is the only reader, so switching it off skips this
///          pass entirely.

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
///          wider starts matching a periodic signal's neighbouring cycle
///          instead of its true arrival offset, and eats into the overlap that
///          pays for the estimate: the search takes its lag range off each end
///          of a fixed-length excerpt, so a wider one is measured over less
///          material rather than over more.
inline constexpr float kDefaultMaxLagMs = 30.0f;

/// @brief Hard ceiling on the searched half-width, in milliseconds.
/// @details 100 ms is a path difference of about 34 m. An offset larger than
///          that is an edit or an arrangement decision, not two microphones on
///          one source, and searching for it would invite spurious matches
///          while leaving less of the excerpt to judge them on.
inline constexpr float kMaxSearchLagMs = 100.0f;

/// @brief Default `|r|` below which a pair is reported as unrelated.
/// @details Two tracks that share less than half their energy shape cannot be
///          given a common arrival time with any confidence, and delaying or
///          flipping one of them on that evidence does audible harm. Bleed-only
///          relationships land below this and are deliberately left alone.
inline constexpr float kDefaultMinAbsCorrelation = 0.5f;

/// @brief Default length of the excerpt each pair is measured over, in seconds.
/// @details The search costs `O(window_samples * log window_samples)` per pair
///          and the pair count is quadratic, so the window is the main cost
///          lever. One second of the passage where both tracks are most active
///          is far more than a time offset needs, and it keeps a full mix's
///          worth of pairs to a fraction of a second of work.
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
///          Both tracks are excerpted at the **same time position**, the window
///          where they are jointly most active, because a lag measured between
///          two different passages means nothing. Pairs recorded at different
///          sample rates go unmeasured for the same reason.
///
///          Every field is the measurement as taken: @ref
///          PairAlignment::lag_samples and @ref PairAlignment::polarity_opposed
///          describe the strongest `|r|` peak, and @ref PairAlignment::related
///          alone says whether acting on it is warranted. An unmeasurable pair
///          comes back with its indices filled and everything else at its
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

/// @brief What the last @ref analyze_phase_alignment call on this thread cost.
/// @details The pass is the most expensive thing the assistant does and its
///          cost is invisible in its result: two implementations of the search
///          return the same alignments and differ only in how much work they
///          did to get there. This reports the work, so the shape of the cost
///          can be asserted rather than timed — a wall-clock bound passes on a
///          fast machine for the wrong reason, and it is the *shape* that a
///          caller budgeting against track count depends on.
struct PhaseAlignmentCost {
  /// @brief Pairs the search actually ran over.
  /// @details Below `n * (n - 1) / 2` when pairs were skipped as unmeasurable —
  ///          an unusable track, mismatched sample rates, too short an overlap.
  int measured_pairs = 0;

  /// @brief Product passes over a whole core segment, summed over those pairs.
  /// @details The load-bearing figure. The search evaluates every lag in its
  ///          range, but it does so through one transform pair rather than one
  ///          pass over the core per lag, so this stays at one pass per
  ///          measured pair and **does not grow with @ref
  ///          PhaseAlignmentConfig::max_lag_ms**. Evaluated lag by lag instead
  ///          it would be `2 * lag_range + 1` passes per pair, which is 2881 at
  ///          the default geometry.
  ///
  ///          Any implementation of the search has to account for its own
  ///          core-length passes here. One that does not stops this counter
  ///          measuring anything, and the test that reads it goes green for the
  ///          wrong reason.
  long long core_product_passes = 0;
};

/// @brief Reads the counters the last call on this thread left behind.
/// @details Thread-local, and reset at the start of every call, so a value only
///          describes the call that just returned on the same thread.
PhaseAlignmentCost last_phase_alignment_cost() noexcept;

}  // namespace sonare::mixing::assistant
