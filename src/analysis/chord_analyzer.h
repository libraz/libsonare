#pragma once

/// @file chord_analyzer.h
/// @brief Chord detection and progression analysis.

#include <string>
#include <vector>

#include "analysis/chord_hmm.h"
#include "analysis/chord_templates.h"
#include "core/audio.h"
#include "feature/chroma.h"
#include "util/types.h"

namespace sonare {

/// @brief Constants for chord analysis algorithm.
/// @details Parameters match bpm-detector Python implementation.
namespace chord_constants {
/// @brief Default smoothing window in seconds (2-second moving average).
constexpr float kSmoothingWindowSec = 2.0f;

/// @brief Threshold for preferring tetrad over triad (0.05 higher correlation required).
constexpr float kTetradThreshold = 0.05f;

/// @brief Minimum duration for chord segments in seconds.
constexpr float kMinDurationSec = 0.3f;

/// @brief Default correlation threshold for chord detection.
constexpr float kCorrelationThreshold = 0.5f;
}  // namespace chord_constants

/// @brief Chroma front-end used for chord recognition.
enum class ChromaMethod {
  STFT,
  NNLS,
};

/// @brief Detected chord with timing information.
struct Chord {
  PitchClass root;                  ///< Root pitch class
  ChordQuality quality;             ///< Chord quality
  float start;                      ///< Start time in seconds
  float end;                        ///< End time in seconds
  float confidence;                 ///< Detection confidence [0, 1]
  PitchClass bass = PitchClass::C;  ///< Bass pitch class for inversion notation

  /// @brief Returns chord name as string (e.g., "Cmaj", "Am").
  std::string to_string() const;

  /// @brief Returns duration in seconds.
  float duration() const { return end - start; }
};

/// @brief Configuration for chord analysis.
struct ChordConfig {
  float min_duration = chord_constants::kMinDurationSec;  ///< Minimum chord duration in seconds
  float smoothing_window =
      chord_constants::kSmoothingWindowSec;                  ///< Smoothing window (2.0s default)
  float threshold = chord_constants::kCorrelationThreshold;  ///< Minimum correlation for detection
  bool use_triads_only = false;                              ///< Use only triads (no 7th chords)
  int n_fft = 2048;                                          ///< FFT size for STFT
  int hop_length = 512;                                      ///< Hop length for STFT
  ChromaMethod chroma_method = ChromaMethod::STFT;           ///< Chroma extraction method
  bool use_beat_sync = true;     ///< Use beat-synchronized chord detection
  bool use_hmm = false;          ///< Use Viterbi HMM smoothing over chord candidates
  int hmm_beam_width = 24;       ///< Candidate beam width for HMM smoothing
  bool use_key_context = false;  ///< Bias HMM transitions by key context
  PitchClass key_root = PitchClass::C;
  Mode key_mode = Mode::Major;
  bool detect_inversions = false;  ///< Estimate bass pitch class and emit slash chords
};

/// @brief Chord analyzer for detecting chords from audio.
/// @details Analyzes chroma features to detect chord progressions.
class ChordAnalyzer {
 public:
  /// @brief Constructs chord analyzer from audio.
  /// @param audio Input audio
  /// @param config Chord configuration
  explicit ChordAnalyzer(const Audio& audio, const ChordConfig& config = ChordConfig());

  /// @brief Constructs chord analyzer from pre-computed chroma.
  /// @param chroma Chromagram
  /// @param config Chord configuration
  ChordAnalyzer(const Chroma& chroma, const ChordConfig& config = ChordConfig());

  /// @brief Constructs chord analyzer with beat synchronization.
  /// @param chroma Chromagram
  /// @param beat_times Beat times in seconds
  /// @param config Chord configuration
  ChordAnalyzer(const Chroma& chroma, const std::vector<float>& beat_times,
                const ChordConfig& config = ChordConfig());

  /// @brief Returns detected chords with timing.
  const std::vector<Chord>& chords() const { return chords_; }

  /// @brief Returns number of detected chords.
  size_t count() const { return chords_.size(); }

  /// @brief Returns chord progression as string (e.g., "C - G - Am - F").
  std::string progression_pattern() const;

  /// @brief Returns functional analysis with Roman numerals.
  /// @param key The key for analysis
  /// @return Vector of Roman numeral strings (e.g., "I", "V", "vi", "IV")
  std::vector<std::string> functional_analysis(PitchClass key_root, Mode mode = Mode::Major) const;

  /// @brief Returns chord at a specific time.
  /// @param time Time in seconds
  /// @return Chord at the given time (empty chord if none)
  Chord chord_at(float time) const;

  /// @brief Returns the most common chord.
  Chord most_common_chord() const;

  /// @brief Returns frame-level chord sequence.
  /// @return Vector of chord indices for each frame
  const std::vector<int>& frame_chords() const { return frame_chords_; }

  /// @brief Returns the chord templates used.
  const std::vector<ChordTemplate>& templates() const { return templates_; }

 private:
  /// @brief Result from chord matching.
  struct ChordMatch {
    int index;         ///< Template index
    float confidence;  ///< Correlation score
  };

  void analyze_chords();
  void analyze_chords_beat_sync(const std::vector<float>& beat_times);
  void merge_short_segments();
  int find_best_chord(const float* chroma) const;
  ChordMatch find_best_chord_with_confidence(const float* chroma) const;
  ChordHmmObservation chord_observation(const float* chroma) const;
  ChordHmmConfig hmm_config() const;
  PitchClass estimate_bass_pitch_class(int start_frame, int end_frame,
                                       const ChordTemplate& chord) const;
  std::string chord_to_roman_numeral(const Chord& chord, PitchClass key_root, Mode mode) const;

  std::vector<Chord> chords_;
  std::vector<int> frame_chords_;
  std::vector<ChordTemplate> templates_;
  Chroma chroma_;
  Chroma bass_chroma_;
  ChordConfig config_;
};

/// @brief Quick chord detection function.
/// @param audio Input audio
/// @param config Chord configuration
/// @return Vector of detected chords
std::vector<Chord> detect_chords(const Audio& audio, const ChordConfig& config = ChordConfig());

}  // namespace sonare
