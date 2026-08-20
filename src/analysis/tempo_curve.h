#pragma once

/// @file tempo_curve.h
/// @brief Per-beat local tempo decoding.
///
/// A beat grid alone already carries a local tempo — the reciprocal of each
/// inter-beat interval — but read directly that curve is far too noisy to show
/// or to segment, because every beat-position quantization error appears as a
/// tempo spike. This decodes a smoothed curve instead: each beat is assigned a
/// hidden tempo state from a log-spaced BPM grid by a deterministic Viterbi
/// recursion that balances how well a state's period matches the observed
/// interval against a penalty on abrupt tempo jumps.
///
/// The decoder lives here, in the analysis layer, rather than beside the
/// arrangement tempo bridge that first used it, for two reasons. It needs
/// nothing from the arrangement subsystem — only beats and an onset envelope —
/// so gating it behind that subsystem would have put a curve out of reach of a
/// plain analysis. And sharing one decoder is what makes the curve reported by
/// an analysis and the segments written into a project agree by construction:
/// they are the same numbers, grouped differently, rather than two estimates
/// that happen to be configured alike.
///
/// Deterministic: identical input always produces an identical curve. No
/// clocks, no randomness; ties break toward the lower state index.

#include <vector>

#include "analysis/beat_analyzer.h"

namespace sonare {

/// @brief One inter-beat interval and how far the decoder should trust it.
struct BeatIntervalObservation {
  /// @brief Interval in seconds from one beat to the next.
  double ibi = 0.0;
  /// @brief Activation weight in [0, 1] sampled from the onset envelope.
  /// @details A beat landing on silence carries little evidence about the local
  ///          tempo, so it is down-weighted and the smoothing prior carries
  ///          across it instead of the interval pulling the curve.
  double weight = 1.0;
};

/// @brief Tempo-state grid and stiffness for the per-beat tempo decoder.
struct TempoCurveConfig {
  /// @brief Lowest BPM the state grid covers.
  float bpm_min = 40.0f;
  /// @brief Highest BPM the state grid covers.
  float bpm_max = 240.0f;
  /// @brief Number of log-spaced states between @ref bpm_min and @ref bpm_max.
  /// @details Log spacing makes the transition cost scale-invariant, so a
  ///          half/double jump costs the same anywhere in the grid.
  int tempo_state_count = 64;
  /// @brief Transition penalty weight; larger gives a stiffer, smoother curve.
  float transition_weight = 8.0f;
};

/// @brief Samples an onset envelope at a time, for weighting beat evidence.
/// @param onset_strength Frame-level onset envelope.
/// @param sample_rate Sample rate the envelope was computed at.
/// @param hop_length Hop length the envelope was computed at.
/// @param time_s Time to sample, in seconds.
/// @return The envelope at the nearest frame, 0 when @p time_s falls outside
///         the envelope, or 1 when no envelope is available so that every
///         position is trusted equally.
double onset_activation_at(const std::vector<float>& onset_strength, int sample_rate,
                           int hop_length, double time_s);

/// @brief Builds the interval observations the decoder consumes.
/// @param beats Detected beats, ordered by time.
/// @param onset_strength Frame-level onset envelope, or empty to trust every
///        beat equally.
/// @param sample_rate Sample rate the envelope was computed at.
/// @param hop_length Hop length the envelope was computed at.
/// @return One entry per interval, so `beats.size() - 1` entries, or empty when
///         fewer than two beats were detected.
std::vector<BeatIntervalObservation> build_beat_interval_observations(
    const std::vector<Beat>& beats, const std::vector<float>& onset_strength, int sample_rate,
    int hop_length);

/// @brief Decodes the smoothed tempo state path over the interval observations.
/// @param observations Interval observations from @ref build_beat_interval_observations.
/// @param config Grid and stiffness settings.
/// @return One BPM per observation, so entry `i` is the tempo of the interval
///         that starts at beat `i`. Empty when the input is empty.
/// @details The returned values are grid points, not continuous estimates. They
///          are precise enough to group beats into tempo regions but coarser
///          than the intervals they came from, so a caller needing timing
///          accuracy over a span should re-derive its BPM from the observed
///          intervals across that span rather than averaging these.
std::vector<double> decode_beat_tempo_curve(
    const std::vector<BeatIntervalObservation>& observations,
    const TempoCurveConfig& config = TempoCurveConfig());

/// @brief Decodes a local BPM for every beat.
/// @param beats Detected beats, ordered by time.
/// @param onset_strength Frame-level onset envelope, or empty to trust every
///        beat equally.
/// @param sample_rate Sample rate the envelope was computed at.
/// @param hop_length Hop length the envelope was computed at.
/// @param config Grid and stiffness settings.
/// @return One BPM per beat, indexing in parallel with @p beats, or empty when
///         fewer than two beats were detected.
/// @details A tempo is a property of the interval between two beats, so the
///          last beat has no interval of its own and repeats the tempo of the
///          interval leading into it. Every other entry is the tempo of the
///          interval starting at that beat.
std::vector<float> estimate_beat_local_bpm(const std::vector<Beat>& beats,
                                           const std::vector<float>& onset_strength,
                                           int sample_rate, int hop_length,
                                           const TempoCurveConfig& config = TempoCurveConfig());

}  // namespace sonare
