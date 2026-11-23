#pragma once

/// @file beat_analyzer.h
/// @brief Beat tracking using dynamic programming.

#include <optional>
#include <vector>

#include "analysis/event_time_refinement.h"
#include "core/audio.h"

namespace sonare {

struct DownbeatObservations;

/// @brief Fraction of the reference onset below which a beat carries no onset evidence.
/// @details The reference is the 3rd-highest value so a single burst cannot set it: a +24 dB
///          burst puts 3.1-high edge beats at 0.17 of it (0.05 of the max); noise stays at 0.03.
constexpr float kOnsetEvidenceRelativeThreshold = 0.1f;

/// @brief Detected beat event.
struct Beat {
  /// Beat time in seconds: the start of the evidence frame, moved by the track's refinement
  /// offset where refine_beat_times() found one
  float time;
  int frame;  ///< Beat frame index
  /// @brief Onset-envelope value sampled at @ref frame.
  /// @details A single raw frame, neither windowed nor normalized, so it is
  ///          sensitive to beat-position jitter and its scale depends on the
  ///          material. Keep this for cheap per-beat magnitude readouts.
  float strength;
};

/// @brief Per-beat accent evidence read from the audio, the meter estimate's input.
/// @details Every accent is the loudest 5 ms RMS (peak_short_time_rms()) near the event: at a
///          beat's own refined attack where its evidence window holds one, otherwise around the
///          reported time widened by half an onset hop. A level read rather than an onset-envelope
///          frame, so two equal events read alike whatever their phase against the hop grid and
///          whatever the input level.
struct BeatAccents {
  std::vector<float> beat;  ///< One linear accent per beat
  /// One flag per beat: whether its evidence window holds an attack
  std::vector<char> beat_has_attack;
  /// One accent per gap between consecutive beats, read at the midpoint's own attack, or 0 where
  /// no attack sits there: a sustained level between the beats is not a subdivision
  std::vector<float> subdivision;
};

/// @brief Detected time signature.
struct TimeSignature {
  int numerator;     ///< Beats per measure (e.g., 4 for 4/4)
  int denominator;   ///< Beat unit (e.g., 4 for quarter note)
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief Configuration for beat tracking.
/// @details The default 60-200 BPM search is the one every public tempo entry uses.
struct BeatConfig {
  /// Finite, positive BPM prior that seeds beat tracking if the tempo search finds none.
  /// The result reports measured tempo or NaN.
  float start_bpm = 120.0f;
  float bpm_min = 60.0f;                ///< Minimum BPM to consider (inclusive)
  float bpm_max = 200.0f;               ///< Maximum BPM to consider (inclusive)
  float tightness = 100.0f;             ///< Tightness of beat distribution
  bool trim = true;                     ///< Trim leading/trailing silent beats
  int n_fft = 2048;                     ///< Onset window, samples at 22050 Hz (rate-scaled)
  int hop_length = 512;                 ///< Onset hop, samples of the input buffer
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
  /// @details Beat times are refined against @p audio (refine_beat_times()) before the meter and
  ///          downbeat passes read them.
  explicit BeatAnalyzer(const Audio& audio, const BeatConfig& config = BeatConfig());

  /// @brief Constructs beat analyzer from pre-computed onset strength.
  /// @param onset_strength Onset strength envelope
  /// @param sr Sample rate
  /// @param hop_length Hop length used
  /// @param config Beat configuration
  /// @param shift_frames Centering shift the envelope carries; beat times subtract it
  /// @details Without audio there is no bar-start evidence, so downbeat_phase() is 0.
  BeatAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
               const BeatConfig& config = BeatConfig(), int shift_frames = 0);

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

  /// @brief Returns the beat index the first measure starts on, in [0, numerator).
  /// @details The bar phase selected from bar-start evidence; downbeat_indices()
  ///          are the beats phase, phase + numerator, and so on. 0 when no
  ///          evidence was measured, as for the onset-envelope constructor.
  int downbeat_phase() const { return downbeat_phase_; }

  /// @brief Moves every beat time by one per-track offset towards the steepest energy rises.
  /// @param audio The signal the onset envelope was computed from.
  /// @param half_window_s Half the envelope's analysis window, in seconds.
  /// @details Each beat is refined inside its own evidence frame (event_time_refinement.h); a
  ///          beat whose frame holds a rise coincides with an onset. The offset is the median
  ///          refinement over those beats and applies to every beat and downbeat, so the spacing
  ///          stays the tracker's; it is 0 when fewer than max(4, beats / 4) beats coincide.
  ///          Frames are untouched. The per-beat accents (beat_accents()) are read here too,
  ///          against the moved times. Call it once, before the meter and downbeat passes.
  void refine_beat_times(const Audio& audio, float half_window_s);

  /// @brief Refines events at ascending envelope frames against the signal refine_beat_times()
  ///        was given.
  /// @details Lets onsets read from this analyzer's envelope share the beats' time definition.
  ///          Every entry is empty when refine_beat_times() has not run.
  std::vector<std::optional<float>> refine_event_times(const std::vector<int>& frames) const;

  /// @brief Re-estimates the time signature and re-seeds the downbeats at phase 0.
  /// @param beat_energy Per-beat low-frequency energy, or empty. A beat whose onset evidence a
  ///        louder event masked is identified from it and left out of the meter scoring. It is
  ///        published as beat_low_frequency_observations().
  void estimate_time_signature(const std::vector<float>& beat_energy = {});

  /// @brief Selects the bar phase from per-beat bar-start evidence.
  /// @details Overwrites downbeat_phase() and the downbeats; empty evidence leaves them as seeded.
  void refine_downbeats(const DownbeatObservations& evidence);

  /// @brief Publishes per-beat chord-change evidence without touching the downbeats.
  void set_chord_change_observations(std::vector<float> chord_changes);

  /// @brief Returns the per-beat accents the meter estimate scores.
  /// @details Empty until refine_beat_times() has read them from the audio, as for the
  ///          onset-envelope constructor; the meter estimate then reads Beat::strength.
  const BeatAccents& beat_accents() const { return accents_; }

  /// @brief Returns beat-local low-frequency energy, the accent evidence that
  ///        survives the log-spectral difference onset strength discards.
  /// @details One value per beat, the meter estimate's masked-beat input, or empty when
  ///          the analyzer was built without audio.
  const std::vector<float>& beat_low_frequency_observations() const {
    return low_frequency_observations_;
  }

  /// @brief Returns per-beat chord-change evidence.
  /// @details One value per beat, or empty unless set_chord_change_observations()
  ///          was called — which only happens once chords are analyzed.
  const std::vector<float>& beat_chord_change_observations() const {
    return chord_change_observations_;
  }

  /// @brief Returns estimated BPM from beat intervals, NaN when no tempo was found.
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

  /// @brief Returns the centering shift, in frames, subtracted from beat frames for times.
  int onset_shift_frames() const { return onset_shift_frames_; }

 private:
  void track_beats();
  /// Reads accents_ against the current beat times; @p attacks holds each beat's refined attack.
  void read_accents(const Audio& audio, const std::vector<std::optional<float>>& attacks);

  std::vector<Beat> beats_;
  std::vector<int> downbeat_indices_;
  std::vector<Beat> downbeats_;
  std::vector<float> onset_strength_;
  std::vector<float> low_frequency_observations_;
  std::vector<float> chord_change_observations_;
  BeatAccents accents_;
  float bpm_;
  TimeSignature time_signature_;
  std::vector<TimeSignature> time_signature_candidates_;
  int downbeat_phase_ = 0;
  int sr_;
  int hop_length_;
  int onset_shift_frames_ = 0;
  BeatConfig config_;
  std::optional<EventTimeRefiner> refiner_;
  float refine_half_window_s_ = 0.0f;
};

/// @brief Quick beat detection function.
/// @param audio Input audio
/// @param config Beat configuration
/// @return Vector of beat times in seconds
std::vector<float> detect_beats(const Audio& audio, const BeatConfig& config = BeatConfig());

}  // namespace sonare
