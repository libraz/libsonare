#pragma once

/// @file music_analyzer.h
/// @brief Unified music analysis facade.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <future>
#endif

#include "analysis/beat_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "util/validated.h"

namespace sonare {

/// @brief Progress callback type for analysis progress reporting.
/// @param progress Progress value (0.0 to 1.0)
/// @param stage Current analysis stage name
using ProgressCallback = std::function<void(float progress, const char* stage)>;

/// @brief Callback type for cooperative analysis cancellation.
/// @return true when the current analysis should stop at the next progress boundary.
using CancelCallback = std::function<bool()>;

/// @brief Relationship between a tempo hypothesis and the selected BPM.
enum class BpmCandidateRelation {
  Primary,
  Half,
  Double,
  Other,
};

/// @brief A BPM hypothesis retained from the tempo estimator.
struct BpmCandidateHypothesis {
  float value = 0.0f;       ///< Candidate BPM
  float confidence = 0.0f;  ///< Candidate confidence [0, 1]
  BpmCandidateRelation relation = BpmCandidateRelation::Other;
};

/// @brief Beat-level evidence the downbeat estimator scores.
/// @details Each vector holds one value per entry of AnalysisResult::beats, so
///          they index in parallel with it. These are inputs to the meter and
///          downbeat decision rather than outputs of it, exposed so callers
///          running their own meter work score the same evidence the library
///          does instead of reconstructing a weaker approximation from the
///          frame-level onset envelope. A stream is empty when the analysis
///          could not produce it.
struct BeatObservations {
  /// @brief Beat-local onset-strength window; unlike Beat::strength this is
  ///        aggregated over a window rather than sampled at a single frame.
  std::vector<float> onset_strength;
  /// @brief Beat-local low-frequency energy, the accent evidence a log-spectral
  ///        difference discards. Empty when analysis ran without audio.
  std::vector<float> low_frequency_energy;
  /// @brief Per-beat chord-change evidence. Empty until chords are analyzed.
  std::vector<float> chord_change;
};

/// @brief Complete music analysis result.
struct AnalysisResult {
  float bpm = 0.0f;             ///< Detected BPM
  float bpm_confidence = 0.0f;  ///< BPM detection confidence
  /// @brief Tempo hypotheses from the existing tempogram estimator.
  std::vector<BpmCandidateHypothesis> bpm_candidates;
  Key key;                       ///< Detected key
  TimeSignature time_signature;  ///< Detected time signature
  /// @brief Meter hypotheses from the existing multi-comb meter estimator.
  std::vector<TimeSignature> time_signature_candidates;
  std::vector<Beat> beats;  ///< Beat positions
  /// @brief Indices into @ref beats that fall on a measure start.
  /// @details Not the same length as @ref beats — it holds one entry per
  ///          detected downbeat, and each entry indexes @ref beats, so
  ///          `beats[downbeat_indices[k]]` is the k-th downbeat. Testing a beat
  ///          for downbeat status is a membership check on this list rather
  ///          than a time comparison against a separate downbeat time series.
  std::vector<int> downbeat_indices;
  /// @brief Beat index the first measure starts on, in [0, numerator).
  /// @details The meter estimator's phase, so `downbeat_indices` normally
  ///          begins at this value. It comes from the meter estimate and is not
  ///          re-derived when downbeats are refined from chord and
  ///          low-frequency-energy evidence, so the two can disagree when the
  ///          refinement moves the first measure start.
  int downbeat_phase = 0;
  /// @brief Beat-level evidence behind the downbeat and meter decisions.
  BeatObservations beat_observations;
  std::vector<Chord> chords;      ///< Chord progression
  std::vector<Section> sections;  ///< Song sections
  Timbre timbre;                  ///< Overall timbre
  Dynamics dynamics;              ///< Dynamics information
  RhythmFeatures rhythm;          ///< Rhythm features
  MelodyContour melody;           ///< Melody contour (pitch trajectory + characteristics)
  std::string form;               ///< Song form (e.g., "IABABCO")
};

/// @brief Configuration for music analysis.
struct MusicAnalyzerConfig {
  int n_fft = 2048;                  ///< FFT size
  int hop_length = 512;              ///< Hop length
  float bpm_min = 60.0f;             ///< Minimum BPM
  float bpm_max = 200.0f;            ///< Maximum BPM
  float start_bpm = 120.0f;          ///< Prior BPM estimate
  bool use_triads_only = true;       ///< Use only triads for chord detection (no 7th chords)
  bool use_hpss = true;              ///< Use HPSS for harmonic-only chroma in chord/key detection
  float chroma_highpass_hz = 80.0f;  ///< High-pass filter cutoff for chroma (0 = disabled)
  bool use_bass_weighted =
      true;  ///< Use bass-weighted chroma combination (bpm-detector compatible)
  int chroma_hop_multiplier = 4;         ///< Hop length multiplier for chroma (larger = faster)
  bool use_chord_hmm = false;            ///< Enable Viterbi HMM chord smoothing in unified analysis
  bool use_chord_key_context = false;    ///< Bias chord HMM transitions using the detected key
  int chord_hmm_beam_width = 24;         ///< Candidate beam width for chord HMM smoothing
  bool detect_chord_inversions = false;  ///< Estimate slash-chord bass notes in unified analysis
  bool adaptive_tempo = false;           ///< Track a locally updated tempo prior during beat DP
  int tempo_update_interval_beats = 8;   ///< Local tempo context length in beats
  /// @brief Meter numerators the multi-comb estimator scores.
  /// @details Adding a numerator widens the search; it does not force the
  ///          result. The default is the historical `{3, 4, 6}`, so an analysis
  ///          that does not set this keeps its previous meter.
  std::vector<int> meter_candidate_numerators = {3, 4, 6};
  /// @brief Beat unit reported for the detected meter.
  /// @details The estimator still reports 8 on its own when it resolves a
  ///          compound meter, so this is the unit for everything else.
  int meter_denominator = 4;
};

/// @brief Largest number of meter numerators a flat C options struct can carry.
/// @details The C ABI passes the candidates as a fixed-size array, so the limit
///          is part of the public contract on every surface rather than an
///          implementation detail of one of them.
constexpr int kMaxMeterCandidateNumerators = 16;

/// @brief Smallest meter numerator the estimator can score.
constexpr int kMinMeterCandidateNumerator = 2;

/// @brief Largest meter numerator the estimator can score.
constexpr int kMaxMeterCandidateNumerator = 32;

/// @brief Largest meter denominator (beat unit) that can be requested.
constexpr int kMaxMeterDenominator = 32;

/// @brief Validation rules for @ref MusicAnalyzerConfig.
/// @details Found by argument-dependent lookup from @ref Validated, which is the
///          only construction path for a checked config. @ref MusicAnalyzer holds
///          one, so every surface that builds a config — the C ABI, WASM, and any
///          future binding — is rejected on the same inputs without repeating the
///          rules.
/// @throws SonareException(InvalidParameter) for a non-finite or out-of-range field.
void validate_config(const MusicAnalyzerConfig& config);

/// @brief Unified music analysis facade.
/// @details Provides lazy access to all analysis modules and a combined analysis result.
/// Each analyzer is created on first access and cached for subsequent use.
class MusicAnalyzer {
 public:
  /// @brief Constructs music analyzer from audio.
  /// @param audio Input audio
  /// @param config Analysis configuration, validated before it is stored
  /// @throws SonareException(InvalidParameter) when @p config is rejected by
  ///         @ref validate_config, or when @p audio is empty.
  explicit MusicAnalyzer(const Audio& audio,
                         const MusicAnalyzerConfig& config = MusicAnalyzerConfig());

  /// @brief Sets progress callback for analysis progress reporting.
  /// @param callback Callback function receiving (progress, stage) parameters
  void set_progress_callback(ProgressCallback callback);

  /// @brief Sets the cooperative cancellation callback used by @ref analyze_cancellable.
  /// @param should_cancel Callback returning true to stop after a progress report.
  void set_cancel_callback(CancelCallback should_cancel);

  // Quick access methods
  /// @brief Returns estimated BPM.
  float bpm();

  /// @brief Returns detected key.
  Key key();

  /// @brief Returns beat times in seconds.
  std::vector<float> beat_times();

  /// @brief Returns detected chords.
  std::vector<Chord> chords();

  /// @brief Returns song form string.
  std::string form();

  // Analyzer access (lazy initialization)
  /// @brief Returns BPM analyzer.
  BpmAnalyzer& bpm_analyzer();

  /// @brief Returns key analyzer.
  KeyAnalyzer& key_analyzer();

  /// @brief Returns beat analyzer.
  /// @note The downbeats are preliminary on first access: they are refined from
  ///       low-frequency-energy and beat-strength evidence only. Chord-change
  ///       evidence is folded in lazily when chord_analyzer() is first invoked,
  ///       which re-refines the same BeatAnalyzer. For chord-informed downbeats,
  ///       call chord_analyzer() (or analyze()) before reading downbeats().
  BeatAnalyzer& beat_analyzer();

  /// @brief Returns chord analyzer.
  ChordAnalyzer& chord_analyzer();

  /// @brief Returns onset analyzer.
  OnsetAnalyzer& onset_analyzer();

  /// @brief Returns dynamics analyzer.
  DynamicsAnalyzer& dynamics_analyzer();

  /// @brief Returns rhythm analyzer.
  RhythmAnalyzer& rhythm_analyzer();

  /// @brief Returns timbre analyzer.
  TimbreAnalyzer& timbre_analyzer();

  /// @brief Returns melody analyzer.
  MelodyAnalyzer& melody_analyzer();

  /// @brief Returns section analyzer.
  SectionAnalyzer& section_analyzer();

  /// @brief Returns boundary detector.
  BoundaryDetector& boundary_detector();

  /// @brief Performs complete analysis and returns result.
  AnalysisResult analyze();

  /// @brief Performs complete analysis and returns no result when cancelled.
  /// @details Cancellation is checked immediately after each existing progress
  ///          report. The regular @ref analyze path remains unchanged.
  std::optional<AnalysisResult> analyze_cancellable();

  /// @brief Returns the input audio.
  const Audio& audio() const { return audio_; }

  /// @brief Returns the configuration.
  const MusicAnalyzerConfig& config() const { return config_.get(); }

 private:
  /// @brief Reports progress to callback if set.
  void report_progress(float progress, const char* stage);

  /// @brief Performs the analysis, optionally checking cooperative cancellation.
  /// @tparam CheckCancel Keeps the regular path free of cancellation branches.
  template <bool CheckCancel>
  std::optional<AnalysisResult> analyze_impl();

  /// @brief Eagerly precomputes feature caches in parallel on native builds.
  void precompute_features();

  Audio audio_;
  Audio analysis_audio_;  ///< Downsampled audio for spectral analysis (22050 Hz)
  int analysis_sr_;       ///< Sample rate of analysis_audio_
  Validated<MusicAnalyzerConfig> config_;
  ProgressCallback progress_callback_;
  CancelCallback cancel_callback_;

#ifndef __EMSCRIPTEN__
  std::mutex progress_mutex_;
#endif

  // Shared feature caches (lazy-initialized)
  /// @brief Returns cached spectrogram, computing if needed.
  const Spectrogram& spectrogram();

  /// @brief Returns cached chroma, computing if needed.
  const Chroma& chroma();

  /// @brief Returns cached mel spectrogram, computing if needed.
  const MelSpectrogram& mel_spectrogram();

  // Lazy-initialized analyzers
  std::unique_ptr<BpmAnalyzer> bpm_analyzer_;
  std::unique_ptr<KeyAnalyzer> key_analyzer_;
  std::unique_ptr<BeatAnalyzer> beat_analyzer_;
  std::unique_ptr<ChordAnalyzer> chord_analyzer_;
  std::unique_ptr<OnsetAnalyzer> onset_analyzer_;
  std::unique_ptr<DynamicsAnalyzer> dynamics_analyzer_;
  std::unique_ptr<RhythmAnalyzer> rhythm_analyzer_;
  std::unique_ptr<TimbreAnalyzer> timbre_analyzer_;
  std::unique_ptr<MelodyAnalyzer> melody_analyzer_;
  std::unique_ptr<SectionAnalyzer> section_analyzer_;
  std::unique_ptr<BoundaryDetector> boundary_detector_;

  // Cached features
  std::unique_ptr<Spectrogram> spectrogram_;
  std::unique_ptr<Chroma> chroma_;
  std::unique_ptr<Chroma> harmonic_chroma_;  ///< Chroma from harmonic-only audio (HPSS)
  std::unique_ptr<MelSpectrogram> mel_spectrogram_;
  std::vector<float> onset_strength_;
  bool onset_strength_computed_ = false;
  std::vector<float> beat_low_frequency_energy_;

  // One-shot flags guarding each lazy initialization so concurrent first-access
  // from multiple threads cannot race on the unique_ptr writes above.
  std::once_flag bpm_analyzer_once_;
  std::once_flag key_analyzer_once_;
  std::once_flag beat_analyzer_once_;
  std::once_flag chord_analyzer_once_;
  std::once_flag onset_analyzer_once_;
  std::once_flag dynamics_analyzer_once_;
  std::once_flag rhythm_analyzer_once_;
  std::once_flag timbre_analyzer_once_;
  std::once_flag melody_analyzer_once_;
  std::once_flag section_analyzer_once_;
  std::once_flag boundary_detector_once_;
  std::once_flag spectrogram_once_;
  std::once_flag chroma_once_;
  std::once_flag harmonic_chroma_once_;
  std::once_flag mel_spectrogram_once_;
  std::once_flag onset_strength_once_;
  std::once_flag beat_low_frequency_energy_once_;

  /// @brief Returns cached onset strength, computing if needed.
  const std::vector<float>& onset_strength();

  /// @brief Returns beat-local low-frequency energy, computing if needed.
  /// @details Filtering the whole signal is the dominant cost of downbeat
  ///          refinement, and both refinement passes see the same beats and the
  ///          same audio, so the observations are computed once and reused.
  const std::vector<float>& beat_low_frequency_energy();

  /// @brief Returns harmonic-only chroma for chord/key detection.
  const Chroma& harmonic_chroma();
};

}  // namespace sonare
