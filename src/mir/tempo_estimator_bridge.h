#pragma once

/// @file tempo_estimator_bridge.h
/// @brief Bridges existing offline beat/downbeat/BPM/meter analysis into
///        transport::TempoSegment / TimeSignatureSegment candidates.
///
/// This is an OFFLINE, control-plane-only component (subsystem sonare::mir).
/// It never runs on the audio thread. It does NOT re-implement beat tracking;
/// it consumes the structs produced by sonare::BeatAnalyzer / MeterAnalyzer /
/// BpmAnalyzer and turns the detected beat grid into the piecewise tempo /
/// time-signature representation consumed by transport::TempoMap.
///
/// @section mir_determinism Determinism
/// The same analysis input always produces byte-identical segment vectors.
/// No clocks, no std::rand, no floating non-determinism beyond IEEE arithmetic.
/// The smoothing pass below is a self-contained deterministic dynamic program.
///
/// @section mir_postproc Beat-activation post-processing
/// We stabilize tempo and phase with a deterministic Viterbi / DBN-style
/// dynamic program over the inter-beat-interval (IBI) sequence (or, when a
/// beat-activation / onset-strength curve is supplied, over activation-weighted
/// IBIs). Each beat index is assigned a hidden "tempo state" drawn from a
/// discrete grid of BPM hypotheses; the Viterbi recursion balances an
/// observation cost (how well a state's period matches the locally observed
/// IBI, weighted by activation strength) against a transition cost that
/// penalizes abrupt tempo jumps between adjacent beats. The decoded state path
/// is the smoothed local-tempo curve, which we then segment into constant /
/// linearly-ramped TempoSegments.
///
/// This is inspired by the dynamic-Bayesian-network beat/downbeat trackers of
/// Korzeniowski, Böck & Widmer 2014 ("Probabilistic Extraction of Beat
/// Positions from a Beat Activation Function") and Böck, Krebs & Widmer 2016
/// ("Joint Beat and Downbeat Tracking with Recurrent Neural Networks"), reduced
/// to a deterministic DP with NO learned model and NO randomness so the bridge
/// stays self-contained and reproducible.
///
/// @section mir_octave half / double tempo ambiguity
/// Tempo octave errors (half / double) are the dominant failure mode of any
/// beat tracker. Rather than silently committing to one octave, the bridge
/// returns a list of TempoEstimate candidates: the primary (decoded) estimate
/// plus the half-tempo and double-tempo variants, each with its own confidence.
/// The caller confirms one via a project edit command; the bridge never mutates a
/// Project.

#include <vector>

#include "analysis/beat_analyzer.h"
#include "transport/tempo_map.h"

namespace sonare::mir {

/// @brief Adapter carrying the offline analysis output the bridge consumes.
///
/// This mirrors the public outputs of sonare::BeatAnalyzer (beats[], bpm,
/// downbeat indices, onset strength, sample rate, hop length) plus the meter
/// from sonare::MeterAnalyzer / BeatAnalyzer::time_signature(). All fields are
/// plain values so callers can populate it from any source (a real analyzer or
/// a synthetic test fixture).
struct BeatAnalysisInput {
  /// Detected beats (time in seconds, optional per-beat strength).
  std::vector<Beat> beats;
  /// Global BPM estimate from BpmAnalyzer / BeatAnalyzer::bpm().
  float bpm = 120.0f;
  /// Beat indices (into `beats`) that are downbeats. Optional; when empty the
  /// bridge derives a phase-0 downbeat grid from the time signature.
  std::vector<int> downbeat_indices;
  /// Estimated time signature (numerator / denominator).
  TimeSignature time_signature{4, 4, 0.0f};
  /// Optional frame-level onset strength / beat-activation envelope. When
  /// present it weights the per-beat observation cost in the DP.
  std::vector<float> onset_strength;
  /// Sample rate the analysis was run at (used to map onset-strength frames to
  /// beat times and to populate TempoSegment::start_sample).
  int sample_rate = 22050;
  /// Hop length of the onset-strength envelope (frames -> seconds).
  int hop_length = 512;
};

/// @brief Configuration for the tempo-estimation bridge.
struct TempoEstimatorConfig {
  /// Discrete tempo-state grid for the Viterbi smoothing pass.
  float bpm_min = 40.0f;
  float bpm_max = 240.0f;
  /// Number of discrete tempo states between bpm_min and bpm_max (log-spaced).
  int tempo_state_count = 64;
  /// Transition penalty weight: larger => smoother (stiffer) tempo curve.
  float transition_weight = 8.0f;
  /// Relative tempo change (fraction) above which two adjacent smoothed beats
  /// are emitted as a tempo RAMP boundary rather than folded into a constant
  /// segment. Also the merge threshold for piecewise-constant segments.
  float ramp_threshold = 0.02f;
  /// PPQ resolution: ppq per quarter note. TempoMap treats 1 ppq == 1 quarter
  /// note, so this is 1.0; exposed for clarity / future sub-quarter grids.
  double ppq_per_beat = 1.0;
  /// Whether to emit half-tempo and double-tempo alternative candidates.
  bool include_octave_candidates = true;
};

/// @brief One tempo / time-signature hypothesis ready for TempoMap.
struct TempoEstimate {
  /// Tempo segments (start_ppq, bpm, start_sample, optional end_bpm ramp),
  /// directly consumable by transport::TempoMap::set_segments.
  std::vector<transport::TempoSegment> segments;
  /// Time-signature segments, consumable by set_time_signatures.
  std::vector<transport::TimeSignatureSegment> time_sigs;
  /// Confidence in [0, 1]. The primary estimate scores highest; half/double
  /// variants are discounted.
  float confidence = 0.0f;
  /// Human-readable label, e.g. "primary", "half", "double".
  const char* label = "primary";
};

/// @brief Runs the bridge and returns ranked tempo/time-signature candidates.
///
/// The first element is the primary (decoded) estimate. When
/// config.include_octave_candidates is set and the beat grid is non-trivial,
/// the half-tempo and double-tempo variants follow, ordered by confidence
/// (descending). Deterministic: identical input -> identical output.
///
/// @param input Offline analysis output adapter.
/// @param config Bridge configuration.
/// @return Ranked candidate list (never empty for a valid input).
std::vector<TempoEstimate> estimate_tempo(
    const BeatAnalysisInput& input, const TempoEstimatorConfig& config = TempoEstimatorConfig());

/// @brief Convenience: build a BeatAnalysisInput from a populated BeatAnalyzer.
BeatAnalysisInput make_input_from_analyzer(const BeatAnalyzer& analyzer);

}  // namespace sonare::mir
