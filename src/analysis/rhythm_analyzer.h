#pragma once

/// @file rhythm_analyzer.h
/// @brief Rhythm analysis including time signature and groove detection.

#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "core/audio.h"

namespace sonare {

/// @brief Rhythm characteristics extracted from audio.
struct RhythmFeatures {
  TimeSignature time_signature;  ///< Detected time signature
  float syncopation;             ///< Syncopation level [0, 1]
  std::string groove_type;       ///< Groove type (straight, shuffle, swing)
  float pattern_regularity;      ///< How regular the rhythm pattern is [0, 1]
  float tempo_stability;         ///< Tempo stability over time [0, 1]
};

/// @brief Configuration for rhythm analysis.
struct RhythmConfig {
  float start_bpm = 120.0f;       ///< Prior estimate for tempo
  float bpm_min = 60.0f;          ///< Minimum BPM to consider
  float bpm_max = 200.0f;         ///< Maximum BPM to consider
  int n_fft = 2048;               ///< FFT size
  int hop_length = 512;           ///< Hop length
  float swing_threshold = 0.15f;  ///< Threshold for detecting swing
};

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
  float syncopation() const { return features_.syncopation; }

  /// @brief Returns groove type ("straight", "shuffle", "swing").
  const std::string& groove_type() const { return features_.groove_type; }

  /// @brief Returns pattern regularity [0, 1].
  float pattern_regularity() const { return features_.pattern_regularity; }

  /// @brief Returns tempo stability [0, 1].
  float tempo_stability() const { return features_.tempo_stability; }

  /// @brief Returns estimated BPM.
  float bpm() const { return bpm_; }

  /// @brief Returns beat intervals in seconds.
  const std::vector<float>& beat_intervals() const { return beat_intervals_; }

 private:
  void analyze();
  void detect_time_signature();
  void detect_groove_type();
  void compute_syncopation();
  void compute_regularity();

  RhythmFeatures features_;
  std::vector<Beat> beats_;
  std::vector<float> beat_intervals_;
  float bpm_;
  RhythmConfig config_;
  int sr_;
  int hop_length_;
};

}  // namespace sonare
