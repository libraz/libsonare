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
  std::vector<PitchPoint> pitches;   ///< Pitch trajectory
  float pitch_range_octaves = 0.0f;  ///< Range of melody in octaves
  float pitch_stability = 0.0f;      ///< Pitch stability [0, 1] (1 = very stable)
  float mean_frequency = 0.0f;       ///< Mean pitch frequency in Hz
  float vibrato_rate = 0.0f;         ///< Vibrato rate in Hz (0 if no vibrato)
};

/// @brief Configuration for melody analysis.
struct MelodyConfig {
  float fmin = 65.0f;       ///< Minimum frequency in Hz (C2)
  float fmax = 2093.0f;     ///< Maximum frequency in Hz (C7)
  int frame_length = 2048;  ///< Frame length in samples
  int hop_length = 256;     ///< Hop length in samples
  float threshold = 0.1f;   ///< YIN threshold (lower = stricter)

  /// @brief Use the pYIN tracker (Viterbi-smoothed) instead of plain per-frame
  ///        YIN. Defaults to false to preserve the historical contour; set to
  ///        true for a less octave-jumpy contour aligned with sonare::pyin.
  bool use_pyin = false;
  /// @brief When @ref use_pyin is true, reflect-pad by frame_length/2 before
  ///        framing so frame i is centered at i*hop_length (matches
  ///        librosa.pyin(center=True)). Ignored by the plain-YIN path, whose
  ///        frame i covers [i*hop_length, i*hop_length+frame_length) (left
  ///        aligned, NOT centered).
  bool center = true;
};

/// @brief Melody analyzer using YIN (or, optionally, pYIN) pitch detection.
/// @details Detects pitch from monophonic audio and extracts melody
/// characteristics.
///
/// @note Divergence from librosa: by default (`use_pyin = false`) this uses
/// plain per-frame YIN with NO Viterbi smoothing and NO frame centering, so the
/// contour is noisier / more octave-jumpy than `librosa.pyin` and frame
/// timestamps are LEFT-aligned (`time = start/sr`), not centered. Set
/// `use_pyin = true` (and keep `center = true`) for a contour and timestamps
/// that match `librosa.pyin(center=True)` semantics.
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
  void compute_contour_features();

  MelodyContour contour_;
  MelodyConfig config_;
  int sr_;
};

}  // namespace sonare
