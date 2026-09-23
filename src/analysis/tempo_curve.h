#pragma once

/// @file tempo_curve.h
/// @brief Per-beat local tempo decoding.
///
/// A beat grid alone already carries a local tempo — the reciprocal of each
/// inter-beat interval — but read directly that curve is far too noisy to show
/// or to segment, because every beat-position quantization error appears as a
/// tempo spike. This decodes a smoothed curve instead: the continuous log-tempo
/// path that minimises the weighted squared log-ratio to each observed interval
/// plus a quadratic penalty on beat-to-beat tempo change. The objective is
/// quadratic, so its exact minimiser comes from one tridiagonal solve.
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
/// clocks, no randomness.

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

/// @brief Tempo range and stiffness for the per-beat tempo decoder.
struct TempoCurveConfig {
  /// @brief Lowest BPM the decoded curve may take.
  float bpm_min = 40.0f;
  /// @brief Highest BPM the decoded curve may take.
  float bpm_max = 240.0f;
  /// @brief Penalty on squared log-tempo change between adjacent intervals;
  ///        larger gives a stiffer, smoother curve.
  /// @details In log tempo the penalty is scale-invariant, so a half/double
  ///          jump costs the same at any tempo.
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

/// @brief Decodes the smoothed tempo path over the interval observations.
/// @param observations Interval observations from @ref build_beat_interval_observations.
/// @param config Grid and stiffness settings.
/// @return One BPM per observation, so entry `i` is the tempo of the interval
///         that starts at beat `i`. Empty when the input is empty.
/// @details Values are continuous, clamped to [bpm_min, bpm_max]. Each is a
///          weighted local average of the neighbouring intervals, so it lags a
///          tempo change by a few beats and a caller needing a span's exact
///          duration should re-derive its BPM from the intervals across that
///          span rather than averaging these.
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
