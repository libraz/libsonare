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
  PitchClass root;  ///< Root pitch class
  Mode mode;        ///< Major or Minor
  /// @brief Share of the model's belief that this key is the answer, in [0, 1).
  /// @details A softmax over the profile correlations of every candidate the
  ///          analyzer scored, so the value falls as the runner-up closes in and
  ///          two keys that split the evidence — a relative major and minor,
  ///          typically — each report about half. The candidate confidences of
  ///          one analysis sum to 1.
  ///
  /// @warning This is the model's own belief, not a measured accuracy. It says
  ///          how decisively the chroma picked this key out of the candidate
  ///          set; it does not say how often that pick is right, and nothing
  ///          here has been fitted against annotated recordings. A confident
  ///          wrong answer is entirely possible, so a pipeline that branches on
  ///          this must choose its own threshold against its own material.
  ///          `tests/fixtures/music_eval/README.md` describes how to measure
  ///          accuracy on a corpus you hold.
  ///
  ///          @ref estimate_key_from_chords and @ref refine_key_with_chords
  ///          report a different quantity in this field — the diatonic share of
  ///          the progression — which is documented at each of them.
  float confidence;

  /// @brief Returns key name (e.g., "C major", "A minor").
  std::string to_string() const;

  /// @brief Returns short key name (e.g., "C", "Am").
  std::string to_short_string() const;
};

/// @brief Key candidate with correlation score.
struct KeyCandidate {
  Key key;            ///< Key information, including its posterior confidence
  float correlation;  ///< Correlation with profile [-1, 1]
};

/// @brief Configuration for key analysis.
struct KeyConfig {
  int n_fft = 4096;                ///< FFT size for chroma
  int hop_length = 512;            ///< Hop length for chroma
  bool use_hpss = false;           ///< Use HPSS to extract harmonic component
  bool loudness_weighted = false;  ///< Weight chroma frames by RMS loudness
  float high_pass_hz = 0.0f;       ///< High-pass filter cutoff (0 = disabled)
  KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler;
  std::string genre_hint = "auto";  ///< "auto" | "edm" | "pop" | "classical" | "jazz"
  std::vector<Mode> modes = {Mode::Major, Mode::Minor};  ///< Candidate modes; default compatible
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

  /// @brief Returns the detected key's posterior confidence; see @ref Key::confidence.
  float confidence() const { return key_.confidence; }

  /// @brief Returns how strong the chroma evidence for the winning key is.
  /// @details Blends the winner's raw correlation with its margin over the
  ///          runner-up. Unlike @ref confidence it is not a share of a
  ///          distribution, so it stays on one scale across analyses that
  ///          scored different numbers of candidates — a posterior mechanically
  ///          shrinks when the search widens, whether or not the evidence
  ///          changed. That is what makes this, not the confidence, the
  ///          quantity the analyzer compares when choosing between chroma
  ///          front-ends. Still not a probability, and not calibrated against
  ///          annotated material.
  float evidence_score() const { return evidence_score_; }

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

  /// @brief Turns the candidate correlations into a softmax distribution.
  void assign_posterior_confidences();

  Key key_;
  float evidence_score_ = 0.0f;
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
/// @return Estimated key. Its @ref Key::confidence is the winning key's share of
///         the progression's total duration once cadence and bookend bonuses are
///         counted — a coverage figure, not the posterior @ref KeyAnalyzer
///         reports and not comparable with it.
Key estimate_key_from_chords(const std::vector<struct Chord>& chords);

/// @brief Refines key estimate using chord progression.
/// @details Combines chroma-based key detection with chord progression analysis.
/// @param chroma_key Key from chroma analysis, carrying the posterior confidence
/// @param chords Detected chords
/// @return Refined key estimate. The confidence it carries is whichever of the
///         two inputs won, so it is on that input's scale.
/// @warning The two inputs measure different things — a posterior over key
///          candidates against a diatonic-coverage share — and this arbitrates
///          between them with fixed thresholds rather than a common scale.
Key refine_key_with_chords(const Key& chroma_key, const std::vector<struct Chord>& chords);

}  // namespace sonare
