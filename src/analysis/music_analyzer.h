#pragma once

/// @file music_analyzer.h
/// @brief Unified music analysis facade.

#include <functional>
#include <memory>
#include <string>
#include <vector>

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

namespace sonare {

/// @brief Progress callback type for analysis progress reporting.
/// @param progress Progress value (0.0 to 1.0)
/// @param stage Current analysis stage name
using ProgressCallback = std::function<void(float progress, const char* stage)>;

/// @brief Complete music analysis result.
struct AnalysisResult {
  float bpm;                      ///< Detected BPM
  float bpm_confidence;           ///< BPM detection confidence
  Key key;                        ///< Detected key
  TimeSignature time_signature;   ///< Detected time signature
  std::vector<Beat> beats;        ///< Beat positions
  std::vector<Chord> chords;      ///< Chord progression
  std::vector<Section> sections;  ///< Song sections
  Timbre timbre;                  ///< Overall timbre
  Dynamics dynamics;              ///< Dynamics information
  RhythmFeatures rhythm;          ///< Rhythm features
  std::string form;               ///< Song form (e.g., "IABABCO")
};

/// @brief Configuration for music analysis.
struct MusicAnalyzerConfig {
  int n_fft = 2048;          ///< FFT size
  int hop_length = 512;      ///< Hop length
  float bpm_min = 60.0f;     ///< Minimum BPM
  float bpm_max = 200.0f;    ///< Maximum BPM
  float start_bpm = 120.0f;  ///< Prior BPM estimate
};

/// @brief Unified music analysis facade.
/// @details Provides lazy access to all analysis modules and a combined analysis result.
/// Each analyzer is created on first access and cached for subsequent use.
class MusicAnalyzer {
 public:
  /// @brief Constructs music analyzer from audio.
  /// @param audio Input audio
  /// @param config Analysis configuration
  explicit MusicAnalyzer(const Audio& audio,
                         const MusicAnalyzerConfig& config = MusicAnalyzerConfig());

  /// @brief Sets progress callback for analysis progress reporting.
  /// @param callback Callback function receiving (progress, stage) parameters
  void set_progress_callback(ProgressCallback callback);

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

  /// @brief Returns the input audio.
  const Audio& audio() const { return audio_; }

  /// @brief Returns the configuration.
  const MusicAnalyzerConfig& config() const { return config_; }

 private:
  /// @brief Reports progress to callback if set.
  void report_progress(float progress, const char* stage);

  Audio audio_;
  MusicAnalyzerConfig config_;
  ProgressCallback progress_callback_;

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
  std::unique_ptr<MelSpectrogram> mel_spectrogram_;
  std::vector<float> onset_strength_;
  bool onset_strength_computed_ = false;

  /// @brief Returns cached onset strength, computing if needed.
  const std::vector<float>& onset_strength();
};

}  // namespace sonare
