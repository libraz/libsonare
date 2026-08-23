#pragma once

/// @file chord_analyzer.h
/// @brief Chord detection and progression analysis.

#include <array>
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

/// @brief Extra correlation an extended-vocabulary quality must beat the triad by.
/// @details Wider than @ref kTetradThreshold because the extended qualities are
/// the confusable ones: a sixth chord spells the same pitch classes as a
/// seventh rooted a third away, and an altered dominant adds one tone to a
/// chord a plain seventh already explains.
constexpr float kExtendedQualityThreshold = 0.09f;

/// @brief Weight of the bass-register root evidence in template selection.
/// @details Scales a [-1, 1] salience term — the candidate root's share of the
/// low-register energy, centred on the share an evenly spread bass would give
/// it — added to the template correlation when choosing between candidates.
/// Folding 12 pitch classes into one vector leaves a chord
/// and its relative — A and F#m share two of three tones — with almost the same
/// chroma evidence and no cue for which note is the root; the bass register
/// carries that cue.
///
/// Deliberately a tie-breaker rather than an override. The bass names the root
/// only in root position, so a weight large enough to overturn a clear chroma
/// decision would relabel every inversion after its own bass note. Zero
/// disables the term.
constexpr float kBassRootWeight = 0.15f;
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

  /// @brief Returns chord name as string (e.g., "C", "Am").
  std::string to_string() const;

  /// @brief Returns duration in seconds.
  float duration() const { return end - start; }
};

/// @brief Configuration for chord analysis.
struct ChordConfig {
  float min_duration = chord_constants::kMinDurationSec;  ///< Minimum chord duration in seconds
  float smoothing_window =
      chord_constants::kSmoothingWindowSec;                  ///< Smoothing window (2.0s default)
  float threshold = chord_constants::kCorrelationThreshold;  ///< Minimum correlation [0, 1]
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
  /// @brief Weight of the bass-register root cue in template selection.
  /// @details Applied only when a bass chromagram is available — either
  /// supplied through the four-argument constructor or computed by the audio
  /// constructor when @ref use_bass_chroma is set. Set to 0 to select purely on
  /// the harmonic chroma. The term biases *which* template is selected; the
  /// reported @ref Chord::confidence stays the selected template's plain chroma
  /// correlation, so the threshold keeps comparing like with like.
  float bass_root_weight = chord_constants::kBassRootWeight;
  /// @brief Compute a low-register chromagram in the audio constructor.
  /// @details Costs one extra bass-band CQT. The constructor also computes one
  /// when @ref detect_inversions is set, and the two share it.
  bool use_bass_chroma = true;
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

  /// @brief Constructs chord analyzer with beat synchronization and a dedicated
  /// bass chromagram for inversion detection.
  /// @param chroma Chromagram used for chord-quality matching
  /// @param beat_times Beat times in seconds
  /// @param bass_chroma Low-register chromagram used to estimate the bass pitch
  ///   class when @c config.detect_inversions is enabled. Pass an empty Chroma to
  ///   fall back to @p chroma.
  /// @param config Chord configuration
  ChordAnalyzer(const Chroma& chroma, const std::vector<float>& beat_times,
                const Chroma& bass_chroma, const ChordConfig& config = ChordConfig());

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
  /// @return Vector of chord-template indices for each frame; @c -1 denotes
  ///   N.C. (the final selected template correlation was below @c threshold).
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
  ChordMatch find_best_chord_with_confidence(const float* chroma,
                                             const std::array<float, 12>* bass = nullptr) const;
  ChordHmmObservation chord_observation(const float* chroma,
                                        const std::array<float, 12>* bass = nullptr) const;

  /// @brief Mean bass-chromagram energy over a span of @c chroma_ frame indices.
  /// @param start_frame First frame, in @c chroma_'s frame space
  /// @param end_frame One past the last frame, in @c chroma_'s frame space
  /// @param out Receives the mean bass chroma
  /// @return @c false when no bass chromagram is available or the span is empty,
  ///         in which case @p out is left untouched.
  bool bass_energy(int start_frame, int end_frame, std::array<float, 12>& out) const;

  /// @brief Salience of @p root in a bass chroma vector, in [-1, 1].
  /// @details 1 when the root is the strongest bass pitch class, 0 for a flat
  /// bass that names no root, negative when the root is weaker than average.
  float bass_root_salience(const std::array<float, 12>& bass, PitchClass root) const;

  /// @brief Selection score for one template: chroma correlation plus the bass cue.
  float template_score(const ChordTemplate& chord_template, const float* chroma,
                       const std::array<float, 12>* bass) const;
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
