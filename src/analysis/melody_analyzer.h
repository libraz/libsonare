#pragma once

/// @file melody_analyzer.h
/// @brief Melody analysis using YIN pitch detection algorithm.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Detected pitch point with timing and confidence.
struct PitchPoint {
  float time;        ///< Time in seconds
  float frequency;   ///< Frequency in Hz (0 if unvoiced)
  float confidence;  ///< Detection confidence [0, 1]
};

/// @brief Melody contour characteristics.
struct MelodyContour {
  std::vector<PitchPoint> pitches;  ///< Pitch trajectory
  float pitch_range_octaves;        ///< Range of melody in octaves
  float pitch_stability;            ///< Pitch stability [0, 1] (1 = very stable)
  float mean_frequency;             ///< Mean pitch frequency in Hz
  float vibrato_rate;               ///< Vibrato rate in Hz (0 if no vibrato)
};

/// @brief Configuration for melody analysis.
struct MelodyConfig {
  float fmin = 80.0f;       ///< Minimum frequency in Hz
  float fmax = 1000.0f;     ///< Maximum frequency in Hz
  int frame_length = 2048;  ///< Frame length in samples
  int hop_length = 256;     ///< Hop length in samples
  float threshold = 0.1f;   ///< YIN threshold (lower = stricter)
};

/// @brief Melody analyzer using simplified YIN algorithm.
/// @details Detects pitch from monophonic audio and extracts melody characteristics.
class MelodyAnalyzer {
 public:
  /// @brief Constructs melody analyzer from audio.
  /// @param audio Input audio
  /// @param config Melody analysis configuration
  explicit MelodyAnalyzer(const Audio& audio, const MelodyConfig& config = MelodyConfig());

  /// @brief Returns the melody contour.
  const MelodyContour& contour() const { return contour_; }

  /// @brief Returns pitch times in seconds.
  std::vector<float> pitch_times() const;

  /// @brief Returns pitch frequencies in Hz.
  std::vector<float> pitch_frequencies() const;

  /// @brief Returns pitch confidences.
  std::vector<float> pitch_confidences() const;

  /// @brief Returns number of pitch points.
  size_t count() const { return contour_.pitches.size(); }

  /// @brief Returns pitch range in octaves.
  float pitch_range() const { return contour_.pitch_range_octaves; }

  /// @brief Returns pitch stability.
  float stability() const { return contour_.pitch_stability; }

  /// @brief Returns mean frequency.
  float mean_frequency() const { return contour_.mean_frequency; }

  /// @brief Returns whether melody was detected.
  bool has_melody() const { return !contour_.pitches.empty() && contour_.mean_frequency > 0.0f; }

 private:
  void analyze();
  float yin_pitch(const float* samples, int frame_size, int sr) const;
  void compute_difference_function(const float* samples, int frame_size, float* diff) const;
  void cumulative_mean_normalize(float* diff, int size) const;
  int find_threshold_crossing(const float* diff, int size) const;
  float parabolic_interpolation(const float* diff, int size, int tau) const;
  void compute_contour_features();

  MelodyContour contour_;
  MelodyConfig config_;
  int sr_;
};

}  // namespace sonare
