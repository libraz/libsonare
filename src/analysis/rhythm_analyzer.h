#pragma once

/// @file rhythm_analyzer.h
/// @brief Rhythm analysis including time signature and groove detection.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "core/audio.h"
#include "util/db.h"

namespace sonare {

/// @brief Constants for rhythm analysis algorithm.
/// @details Parameters match bpm-detector Python implementation.
namespace rhythm_constants {
/// @brief Tolerance for matching onset positions to grid (10%).
constexpr float kSwingTolerance = 0.1f;

/// @brief Swing eighth note positions (triplet feel: 1/3 and 2/3 of beat).
constexpr std::array<float, 2> kSwingPositions = {0.33f, 0.67f};

/// @brief Straight eighth note positions (even subdivisions).
constexpr std::array<float, 3> kStraightPositions = {0.25f, 0.5f, 0.75f};

/// @brief Threshold for detecting shuffle groove (swing_ratio > 0.55).
constexpr float kShuffleThreshold = 0.55f;

/// @brief Threshold for detecting swing groove (swing_ratio > 0.6).
constexpr float kSwingThreshold = 0.6f;

/// @brief Fewest tracked beats syncopation is read from, one bar of the default meter.
constexpr size_t kMinSyncopationBeats = 4;
}  // namespace rhythm_constants

/// @brief Rhythm characteristics extracted from audio.
struct RhythmFeatures {
  TimeSignature time_signature;  ///< Detected time signature
  float syncopation;             ///< Syncopation level [0, 1]; see RhythmAnalyzer::syncopation()
  std::string groove_type;       ///< Groove type (straight, shuffle, swing)
  float pattern_regularity;      ///< Rhythm regularity [0, 1]; NaN below two beat intervals
  float tempo_stability;         ///< Tempo stability [0, 1]; NaN below two positive beat intervals
};

/// @brief Configuration for rhythm analysis.
struct RhythmConfig {
  /// Finite, positive BPM prior that seeds beat tracking if the tempo search finds none.
  /// The result reports measured tempo or NaN.
  float start_bpm = 120.0f;
  float bpm_min = 60.0f;   ///< Minimum BPM to consider
  float bpm_max = 200.0f;  ///< Maximum BPM to consider
  int n_fft = 2048;        ///< Window length in samples at 22050 Hz, rescaled to the input rate
  int hop_length = 512;    ///< Hop length in samples of the input buffer
  /// @brief Meter numerators handed to the meter estimator.
  /// @details Must be kept in step with BeatConfig's copy: this analyzer runs
  ///          its own meter estimate for RhythmFeatures::time_signature, so a
  ///          candidate set that reaches only one of the two makes the reported
  ///          meter depend on which field a caller happens to read.
  std::vector<int> meter_candidate_numerators = {3, 4, 6};
  int meter_denominator = 4;  ///< Beat unit handed to the meter estimator
};

/// @brief Refuses a RhythmConfig tempo search outside the argument domain every surface shares.
/// @throws SonareException(InvalidParameter) as @ref validate_tempo_search describes.
inline void validate_rhythm_config(const RhythmConfig& config) {
  validate_tempo_search(config.bpm_min, config.bpm_max, config.start_bpm, config.n_fft,
                        config.hop_length);
}

/// @brief Regularity measures derived from a run of beat intervals.
struct RhythmRegularity {
  /// 1 - 2 * interval CV, clamped at 0; needs at least two intervals with a positive mean.
  float pattern_regularity = kNotMeasured;
  /// 1 - 5 * local-tempo CV, clamped at 0; needs at least two positive intervals.
  float tempo_stability = kNotMeasured;
};

/// @brief Computes @ref RhythmRegularity from beat intervals in seconds.
/// @details A field whose requirement does not hold is not measured (NaN), so neither a single
///          interval nor silence reads as a perfectly regular or perfectly irregular rhythm.
RhythmRegularity rhythm_regularity(const std::vector<float>& intervals);

/// @brief Rhythm analyzer for detecting musical rhythm characteristics.
/// @details Analyzes beat patterns to detect time signature, groove type,
/// syncopation level, and other rhythm features.
class RhythmAnalyzer {
 public:
  /// @brief Constructs rhythm analyzer from audio.
  /// @param audio Input audio
  /// @param config Rhythm analysis configuration
  explicit RhythmAnalyzer(const Audio& audio, const RhythmConfig& config = RhythmConfig());

  /// @brief Constructs rhythm analyzer from existing beat analyzer.
  /// @param beat_analyzer Pre-computed beat analyzer
  /// @param config Rhythm analysis configuration
  explicit RhythmAnalyzer(const BeatAnalyzer& beat_analyzer,
                          const RhythmConfig& config = RhythmConfig());

  /// @brief Returns rhythm features.
  const RhythmFeatures& features() const { return features_; }

  /// @brief Returns detected time signature.
  TimeSignature time_signature() const { return features_.time_signature; }

  /// @brief Returns syncopation level [0, 1].
  /// @details Not measured (NaN) unless at least four beats were tracked, the accents are audible,
  ///          and both a strong- and a weak-position beat were observed with audible strong beats.
  float syncopation() const { return features_.syncopation; }

  /// @brief Returns groove type ("straight", "shuffle", "swing").
  const std::string& groove_type() const { return features_.groove_type; }

  /// @brief Returns pattern regularity [0, 1].
  float pattern_regularity() const { return features_.pattern_regularity; }

  /// @brief Returns tempo stability [0, 1].
  float tempo_stability() const { return features_.tempo_stability; }

  /// @brief Returns estimated BPM, NaN when no tempo was found.
  float bpm() const { return bpm_; }

  /// @brief Returns beat intervals in seconds.
  const std::vector<float>& beat_intervals() const { return beat_intervals_; }

  /// @brief Returns the beat index the first measure starts on.
  /// @details The beat analyzer's selected bar phase; bar positions count from it.
  int downbeat_phase() const { return downbeat_phase_; }

 private:
  void analyze();
  void detect_time_signature();
  void detect_groove_type();
  void compute_syncopation();
  void compute_regularity();
  void detect_onsets(const BeatAnalyzer& beat_analyzer);
  float calculate_swing_ratio() const;

  RhythmFeatures features_;
  std::vector<Beat> beats_;
  std::vector<float> beat_intervals_;
  /// @brief The beat analyzer's per-beat accents, empty when it had no audio.
  /// @details Scored by both the meter estimate and the syncopation measure.
  BeatAccents accents_;
  /// @brief Per-beat low-frequency energy from the beat analyzer, empty when it had no audio.
  std::vector<float> beat_low_frequency_energy_;
  /// @brief Beats the meter estimate left out as masked; syncopation skips them too.
  std::vector<int> unobserved_beats_;
  std::vector<float> onset_times_;  ///< Detected onset times in seconds
  float bpm_;
  int downbeat_phase_ = 0;  ///< Beat index of the first downbeat (from the beat analyzer)
  RhythmConfig config_;
  int sr_;
  int hop_length_;
};

}  // namespace sonare
