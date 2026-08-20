#pragma once

/// @file meter_analyzer.h
/// @brief Multi-comb meter analysis for time-signature estimation.

#include <vector>

#include "analysis/beat_analyzer.h"

namespace sonare {

/// @brief Largest number of meter numerators a flat C options struct can carry.
/// @details The C ABI passes the candidates as a fixed-size array, so the limit
///          is part of the public contract on every surface rather than an
///          implementation detail of one of them.
constexpr int kMaxMeterCandidateNumerators = 16;

/// @brief Smallest meter numerator the estimator can score.
constexpr int kMinMeterCandidateNumerator = 2;

/// @brief Largest meter numerator the estimator can score.
constexpr int kMaxMeterCandidateNumerator = 32;

/// @brief Largest meter denominator (beat unit) that can be requested.
constexpr int kMaxMeterDenominator = 32;

/// @brief Widest numerator whose internal grouping is searched.
/// @details A bar is split into groups of two and three beats, and the number
///          of ways to do that grows quickly enough that beyond this width the
///          search is asking a beat series of realistic length to separate
///          alternatives it cannot: 16 already admits 37 groupings, while a
///          span long enough to rank them would have to run to hundreds of
///          bars. Wider numerators are still scored, they are just reported
///          without an internal grouping.
constexpr int kMaxGroupedMeterNumerator = 16;

/// @brief Configuration for multi-comb meter analysis.
struct MeterConfig {
  std::vector<int> candidate_numerators = {3, 4, 6};
  int denominator = 4;
  float downbeat_weight = 1.0f;
  float measure_weight = 0.5f;
  float subdivision_weight = 0.15f;
  float compound_subdivision_threshold = 0.85f;
};

/// @brief Result from meter analysis.
struct MeterResult {
  TimeSignature time_signature{4, 4, 0.0f};
  int downbeat_phase = 0;
  /// @brief Beat counts of the accent groups inside one bar, summing to the numerator.
  /// @details How the bar divides, in beats per group: {3, 2, 2} is the 7/8 an
  ///          aksak meter notates as 3+2+2, and {2, 2} is an ordinary 4. Every
  ///          entry is a 2 or a 3 whenever a grouping was resolved, and the
  ///          entries always sum to the numerator.
  ///
  ///          A single entry means no internal accent structure was resolved,
  ///          which happens for three different reasons: the numerator has none
  ///          to find (2 and 3 divide only one way), it is wider than
  ///          kMaxGroupedMeterNumerator and was not searched, or no search ran
  ///          at all because the span was too short. TimeSignature::confidence
  ///          is what separates the last case from the first two.
  std::vector<int> grouping;
  /// @brief Support per requested candidate numerator, in the requested order.
  /// @details Standardized and signed: zero is the level a numerator reaches on
  ///          beats carrying no meter, so a negative entry means less support
  ///          than noise would produce and only the ordering and the gaps
  ///          between entries carry meaning. Reading one entry on its own says
  ///          nothing.
  std::vector<float> candidate_scores;
  /// @brief Candidate signatures in descending existing multi-comb score order.
  std::vector<TimeSignature> candidates;
};

/// @brief Estimates meter from beat-aligned onset strengths using a multi-comb score.
class MeterAnalyzer {
 public:
  MeterAnalyzer(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                const MeterConfig& config = MeterConfig());

  const MeterResult& result() const { return result_; }
  TimeSignature time_signature() const { return result_.time_signature; }

 private:
  void analyze(const std::vector<float>& onset_strength, const std::vector<Beat>& beats);

  MeterConfig config_;
  MeterResult result_;
};

MeterResult estimate_meter(const std::vector<float>& onset_strength, const std::vector<Beat>& beats,
                           const MeterConfig& config = MeterConfig());

/// @brief Validates a meter configuration, throwing on a value the estimator
///        cannot answer rather than silently substituting a default.
/// @details Lives in the core rather than at a binding boundary so every surface
///          rejects the same input for the same reason, including the ones that
///          call the core directly instead of going through the C ABI.
void validate_meter_config(const MeterConfig& config);

/// @brief Estimates meter over a caller-supplied beat series.
/// @param beat_times Beat positions in seconds, ascending.
/// @param beat_strengths Per-beat accent value, the same length as @p beat_times.
///        AnalysisResult::beat_observations.onset_strength is the intended
///        source; Beat::strength also works but is a single unwindowed frame.
///        Neither needs pre-scaling: the series is divided by its own maximum
///        before scoring, so only the accent contrast within it is read and the
///        absolute units it arrives in do not matter.
/// @param config Scoring configuration; validated before use.
/// @details The scoring reads only the per-beat strengths, so no audio or
///          frame-level onset envelope is needed and a caller can score an
///          arbitrary span of an existing analysis without re-running it.
///          An empty beat series is rejected: there is nothing to score, and
///          answering it with the fixed default would read as a detection.
///          One to seven beats is accepted but is not a search either — it
///          reports that same low-confidence default, so read
///          TimeSignature::confidence before treating a short span's answer as
///          a detection.
MeterResult estimate_meter_from_beats(const std::vector<float>& beat_times,
                                      const std::vector<float>& beat_strengths,
                                      const MeterConfig& config = MeterConfig());

}  // namespace sonare
