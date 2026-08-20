#pragma once

/// @file beat_analyzer.h
/// @brief Beat tracking using dynamic programming.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Detected beat event.
struct Beat {
  float time;  ///< Beat time in seconds
  int frame;   ///< Beat frame index
  /// @brief Onset-envelope value sampled at @ref frame.
  /// @details A single raw frame, neither windowed nor normalized, so it is
  ///          sensitive to beat-position jitter and its scale depends on the
  ///          material. The downbeat refinement deliberately does not use this
  ///          value — it scores a beat-local window instead, exposed as
  ///          BeatAnalyzer::beat_onset_observations(). Prefer that for accent
  ///          scoring, and keep this for cheap per-beat magnitude readouts.
  float strength;
};

/// @brief Detected time signature.
struct TimeSignature {
  int numerator;     ///< Beats per measure (e.g., 4 for 4/4)
  int denominator;   ///< Beat unit (e.g., 4 for quarter note)
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief Configuration for beat tracking.
/// @details Default values follow common MIR defaults.
struct BeatConfig {
  float start_bpm = 120.0f;             ///< Prior estimate for tempo
  float bpm_min = 30.0f;                ///< Minimum BPM to consider
  float bpm_max = 300.0f;               ///< Maximum BPM to consider
  float tightness = 100.0f;             ///< Tightness of beat distribution
  bool trim = true;                     ///< Trim leading/trailing silent beats
  int n_fft = 2048;                     ///< FFT size for onset detection
  int hop_length = 512;                 ///< Hop length for onset detection
  bool adaptive_tempo = false;          ///< Locally update tempo prior during DP
  int tempo_update_interval_beats = 8;  ///< Local tempo context length in beats
  /// @brief Meter numerators handed to the meter estimator.
  /// @details Mirrors MeterConfig::candidate_numerators. It is flattened here
  ///          rather than holding a MeterConfig because meter_analyzer.h
  ///          already depends on this header.
  std::vector<int> meter_candidate_numerators = {3, 4, 6};
  int meter_denominator = 4;  ///< Beat unit handed to the meter estimator
};

/// @brief Beat analyzer using dynamic programming beat tracking.
/// @details Uses onset strength envelope and DP to find optimal beat sequence
/// that maximizes onset alignment while maintaining tempo consistency.
class BeatAnalyzer {
 public:
  /// @brief Constructs beat analyzer from audio.
  /// @param audio Input audio
  /// @param config Beat configuration
  explicit BeatAnalyzer(const Audio& audio, const BeatConfig& config = BeatConfig());

  /// @brief Constructs beat analyzer from pre-computed onset strength.
  /// @param onset_strength Onset strength envelope
  /// @param sr Sample rate
  /// @param hop_length Hop length used
  /// @param config Beat configuration
  BeatAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
               const BeatConfig& config = BeatConfig());

  /// @brief Returns detected beats.
  const std::vector<Beat>& beats() const { return beats_; }

  /// @brief Returns beat times in seconds.
  std::vector<float> beat_times() const;

  /// @brief Returns beat frames (indices).
  std::vector<int> beat_frames() const;

  /// @brief Returns downbeat beat indices.
  const std::vector<int>& downbeat_indices() const { return downbeat_indices_; }

  /// @brief Returns estimated downbeats.
  const std::vector<Beat>& downbeats() const { return downbeats_; }

  /// @brief Returns the beat index the first measure starts on.
  /// @details Set by the meter estimate and left alone by refine_downbeats(),
  ///          so it can disagree with downbeat_indices().front() once the
  ///          refinement has moved the first measure start.
  int downbeat_phase() const { return downbeat_phase_; }

  /// @brief Refines downbeats using optional beat-level observations.
  void refine_downbeats(const std::vector<float>& low_frequency_energy = {},
                        const std::vector<float>& chord_changes = {});

  /// @brief Returns the beat-local onset-strength window the refinement scored.
  /// @details One value per beat, aggregated over a window around the beat
  ///          rather than sampled at a single frame like Beat::strength. Empty
  ///          until refine_downbeats() has run.
  const std::vector<float>& beat_onset_observations() const { return onset_observations_; }

  /// @brief Returns beat-local low-frequency energy, the accent evidence that
  ///        survives the log-spectral difference onset strength discards.
  /// @details One value per beat, or empty when the analyzer was built without
  ///          audio or refine_downbeats() has not run.
  const std::vector<float>& beat_low_frequency_observations() const {
    return low_frequency_observations_;
  }

  /// @brief Returns per-beat chord-change evidence.
  /// @details One value per beat, or empty unless refine_downbeats() was called
  ///          with chord changes — which only happens once chords are analyzed.
  const std::vector<float>& beat_chord_change_observations() const {
    return chord_change_observations_;
  }

  /// @brief Returns estimated BPM from beat intervals.
  float bpm() const { return bpm_; }

  /// @brief Returns estimated time signature.
  TimeSignature time_signature() const { return time_signature_; }

  /// @brief Returns meter candidates retained from the time-signature estimate.
  const std::vector<TimeSignature>& time_signature_candidates() const {
    return time_signature_candidates_;
  }

  /// @brief Returns number of detected beats.
  size_t count() const { return beats_.size(); }

  /// @brief Returns the onset strength envelope used.
  const std::vector<float>& onset_strength() const { return onset_strength_; }

  /// @brief Returns sample rate.
  int sample_rate() const { return sr_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

 private:
  void track_beats();
  void estimate_time_signature(const std::vector<float>& beat_strength_observations = {});
  float compute_transition_cost(int from_frame, int to_frame, float period) const;

  std::vector<Beat> beats_;
  std::vector<int> downbeat_indices_;
  std::vector<Beat> downbeats_;
  std::vector<float> onset_strength_;
  // Retained from the last refine_downbeats() call. These are the scored
  // evidence rather than a derived result, kept so callers doing their own
  // meter or downbeat work can reuse them instead of recomputing a weaker
  // approximation from the frame-level envelope.
  std::vector<float> onset_observations_;
  std::vector<float> low_frequency_observations_;
  std::vector<float> chord_change_observations_;
  float bpm_;
  TimeSignature time_signature_;
  std::vector<TimeSignature> time_signature_candidates_;
  int downbeat_phase_ = 0;
  int sr_;
  int hop_length_;
  BeatConfig config_;
};

/// @brief Rewrites an onset envelope so each frame carries the energy around it.
/// @param onset_strength Frame-level onset envelope.
/// @param sr Sample rate the envelope was computed at.
/// @param hop_length Hop length the envelope was computed at.
/// @return An envelope of the same length, or the input unchanged when it is
///         empty or the rate arguments are not positive.
/// @details A beat whose position falls between two hops splits its energy over
///          neighbouring frames, so a single frame reads it as weaker than an
///          identical beat that lands on a hop boundary. Anything scoring one
///          frame per beat — meter estimation, accent comparison — would report
///          that split as an accent difference, so it compares this envelope
///          instead of the raw one. Where the energy sits, which is what those
///          consumers actually measure, is unchanged.
std::vector<float> beat_local_energy(const std::vector<float>& onset_strength, int sr,
                                     int hop_length);

/// @brief Quick beat detection function.
/// @param audio Input audio
/// @param config Beat configuration
/// @return Vector of beat times in seconds
std::vector<float> detect_beats(const Audio& audio, const BeatConfig& config = BeatConfig());

}  // namespace sonare
