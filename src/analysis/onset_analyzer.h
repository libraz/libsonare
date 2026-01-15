#pragma once

/// @file onset_analyzer.h
/// @brief Onset detection and analysis.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Detected onset event.
struct Onset {
  float time;      ///< Onset time in seconds
  float strength;  ///< Onset strength
};

/// @brief Configuration for onset detection.
/// @details Default values match librosa.onset.onset_detect (sr=22050, hop=512).
struct OnsetDetectConfig {
  int n_fft = 2048;          ///< FFT size
  int hop_length = 512;      ///< Hop length
  float threshold = 0.0f;    ///< Minimum onset strength (0 = adaptive)
  int pre_max = 1;           ///< Frames before peak for local max (~30ms)
  int post_max = 1;          ///< Frames after peak for local max
  int pre_avg = 3;           ///< Frames for pre-onset average (librosa default: 3)
  int post_avg = 4;          ///< Frames for post-onset average (librosa default: 4)
  float delta = 0.06f;       ///< Offset for adaptive threshold (librosa default: 0.06)
  int wait = 1;              ///< Minimum frames between consecutive onsets (~30ms)
  bool backtrack = false;    ///< Backtrack to nearest local minimum
  int backtrack_range = 10;  ///< Maximum backtrack range in frames
};

/// @brief Onset analyzer for detecting note/beat onsets.
class OnsetAnalyzer {
 public:
  /// @brief Constructs onset analyzer from audio.
  /// @param audio Input audio
  /// @param config Onset configuration
  explicit OnsetAnalyzer(const Audio& audio, const OnsetDetectConfig& config = OnsetDetectConfig());

  /// @brief Constructs onset analyzer from pre-computed onset strength.
  /// @param onset_strength Onset strength envelope
  /// @param sr Sample rate
  /// @param hop_length Hop length used
  /// @param config Onset configuration
  OnsetAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
                const OnsetDetectConfig& config = OnsetDetectConfig());

  /// @brief Returns detected onsets.
  const std::vector<Onset>& onsets() const { return onsets_; }

  /// @brief Returns onset times in seconds.
  std::vector<float> onset_times() const;

  /// @brief Returns onset frames (indices).
  std::vector<int> onset_frames() const;

  /// @brief Returns the onset strength envelope.
  const std::vector<float>& onset_strength() const { return onset_strength_; }

  /// @brief Returns number of detected onsets.
  size_t count() const { return onsets_.size(); }

  /// @brief Returns sample rate.
  int sample_rate() const { return sr_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

 private:
  void detect_onsets();
  void backtrack_onsets();

  std::vector<Onset> onsets_;
  std::vector<float> onset_strength_;
  int sr_;
  int hop_length_;
  OnsetDetectConfig config_;
};

/// @brief Quick onset detection function.
/// @param audio Input audio
/// @param config Onset configuration
/// @return Vector of onset times in seconds
std::vector<float> detect_onsets(const Audio& audio,
                                 const OnsetDetectConfig& config = OnsetDetectConfig());

}  // namespace sonare
