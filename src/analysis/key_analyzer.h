#pragma once

/// @file key_analyzer.h
/// @brief Musical key detection.

#include <string>
#include <vector>

#include "analysis/key_profiles.h"
#include "core/audio.h"
#include "feature/chroma.h"
#include "util/types.h"

namespace sonare {

// Forward declaration
struct Chord;

/// @brief Detected musical key.
struct Key {
  PitchClass root;   ///< Root pitch class
  Mode mode;         ///< Major or Minor
  float confidence;  ///< Confidence score [0, 1]

  /// @brief Returns key name (e.g., "C major", "A minor").
  std::string to_string() const;

  /// @brief Returns short key name (e.g., "C", "Am").
  std::string to_short_string() const;
};

/// @brief Key candidate with correlation score.
struct KeyCandidate {
  Key key;            ///< Key information
  float correlation;  ///< Correlation with profile [-1, 1]
};

/// @brief Configuration for key analysis.
struct KeyConfig {
  int n_fft = 4096;           ///< FFT size for chroma
  int hop_length = 512;       ///< Hop length for chroma
  bool use_hpss = false;      ///< Use HPSS to extract harmonic component
  float high_pass_hz = 0.0f;  ///< High-pass filter cutoff (0 = disabled)
  KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler;
};

/// @brief Key analyzer using chroma correlation.
class KeyAnalyzer {
 public:
  /// @brief Constructs key analyzer from audio.
  /// @param audio Input audio
  /// @param config Key configuration
  explicit KeyAnalyzer(const Audio& audio, const KeyConfig& config = KeyConfig());

  /// @brief Constructs key analyzer from pre-computed chroma.
  /// @param chroma Chromagram
  /// @param config Key configuration
  explicit KeyAnalyzer(const Chroma& chroma, const KeyConfig& config = KeyConfig());

  /// @brief Constructs key analyzer from mean chroma vector.
  /// @param mean_chroma Mean chroma [12]
  /// @param config Key configuration
  KeyAnalyzer(const std::array<float, 12>& mean_chroma, const KeyConfig& config = KeyConfig());

  /// @brief Returns the detected key.
  Key key() const { return key_; }

  /// @brief Returns the root pitch class.
  PitchClass root() const { return key_.root; }

  /// @brief Returns the mode (Major/Minor).
  Mode mode() const { return key_.mode; }

  /// @brief Returns confidence of the key estimate [0, 1].
  float confidence() const { return key_.confidence; }

  /// @brief Returns top key candidates.
  /// @param top_n Number of candidates to return
  /// @return Sorted list of key candidates
  std::vector<KeyCandidate> candidates(int top_n = 5) const;

  /// @brief Returns all 24 key candidates (12 major + 12 minor).
  const std::vector<KeyCandidate>& all_candidates() const { return candidates_; }

  /// @brief Returns the mean chroma vector used for analysis.
  const std::array<float, 12>& mean_chroma() const { return mean_chroma_; }

 private:
  void analyze();

  Key key_;
  std::array<float, 12> mean_chroma_;
  std::vector<KeyCandidate> candidates_;
  KeyConfig config_;
};

/// @brief Quick key detection function.
/// @param audio Input audio
/// @param config Key configuration
/// @return Detected key
Key detect_key(const Audio& audio, const KeyConfig& config = KeyConfig());

/// @brief Estimates key from chord progression.
/// @details Uses diatonic chord analysis to determine the most likely key.
/// For progressions like C-G-Am-F, this correctly identifies C major.
/// @param chords Detected chord sequence
/// @return Estimated key with confidence
Key estimate_key_from_chords(const std::vector<struct Chord>& chords);

/// @brief Refines key estimate using chord progression.
/// @details Combines chroma-based key detection with chord progression analysis.
/// @param chroma_key Key from chroma analysis
/// @param chords Detected chords
/// @return Refined key estimate
Key refine_key_with_chords(const Key& chroma_key, const std::vector<struct Chord>& chords);

}  // namespace sonare
