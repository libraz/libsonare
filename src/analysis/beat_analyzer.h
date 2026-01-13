#pragma once

/// @file beat_analyzer.h
/// @brief Beat tracking using dynamic programming.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Detected beat event.
struct Beat {
  float time;      ///< Beat time in seconds
  int frame;       ///< Beat frame index
  float strength;  ///< Beat strength
};

/// @brief Detected time signature.
struct TimeSignature {
  int numerator;     ///< Beats per measure (e.g., 4 for 4/4)
  int denominator;   ///< Beat unit (e.g., 4 for quarter note)
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief Configuration for beat tracking.
/// @details Default values match librosa.
struct BeatConfig {
  float start_bpm = 120.0f;  ///< Prior estimate for tempo
  float bpm_min = 30.0f;     ///< Minimum BPM to consider
  float bpm_max = 300.0f;    ///< Maximum BPM to consider
  float tightness = 100.0f;  ///< Tightness of beat distribution
  bool trim = true;          ///< Trim leading/trailing silent beats
  int n_fft = 2048;          ///< FFT size for onset detection
  int hop_length = 512;      ///< Hop length for onset detection
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

  /// @brief Returns estimated BPM from beat intervals.
  float bpm() const { return bpm_; }

  /// @brief Returns estimated time signature.
  TimeSignature time_signature() const { return time_signature_; }

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
  void estimate_time_signature();
  float compute_transition_cost(int from_frame, int to_frame, float period) const;

  std::vector<Beat> beats_;
  std::vector<float> onset_strength_;
  float bpm_;
  TimeSignature time_signature_;
  int sr_;
  int hop_length_;
  BeatConfig config_;
};

/// @brief Quick beat detection function.
/// @param audio Input audio
/// @param config Beat configuration
/// @return Vector of beat times in seconds
std::vector<float> detect_beats(const Audio& audio, const BeatConfig& config = BeatConfig());

}  // namespace sonare
