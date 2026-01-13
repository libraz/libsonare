#include "analysis/music_analyzer.h"

#include "util/exception.h"

namespace sonare {

MusicAnalyzer::MusicAnalyzer(const Audio& audio, const MusicAnalyzerConfig& config)
    : audio_(audio), config_(config), progress_callback_(nullptr) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
}

void MusicAnalyzer::set_progress_callback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
}

void MusicAnalyzer::report_progress(float progress, const char* stage) {
  if (progress_callback_) {
    progress_callback_(progress, stage);
  }
}

float MusicAnalyzer::bpm() { return bpm_analyzer().bpm(); }

Key MusicAnalyzer::key() { return key_analyzer().key(); }

std::vector<float> MusicAnalyzer::beat_times() { return beat_analyzer().beat_times(); }

std::vector<Chord> MusicAnalyzer::chords() { return chord_analyzer().chords(); }

std::string MusicAnalyzer::form() { return section_analyzer().form(); }

BpmAnalyzer& MusicAnalyzer::bpm_analyzer() {
  if (!bpm_analyzer_) {
    BpmConfig bpm_config;
    bpm_config.bpm_min = config_.bpm_min;
    bpm_config.bpm_max = config_.bpm_max;
    bpm_config.start_bpm = config_.start_bpm;
    bpm_config.n_fft = config_.n_fft;
    bpm_config.hop_length = config_.hop_length;
    bpm_analyzer_ = std::make_unique<BpmAnalyzer>(audio_, bpm_config);
  }
  return *bpm_analyzer_;
}

KeyAnalyzer& MusicAnalyzer::key_analyzer() {
  if (!key_analyzer_) {
    KeyConfig key_config;
    key_config.hop_length = config_.hop_length;
    key_analyzer_ = std::make_unique<KeyAnalyzer>(audio_, key_config);
  }
  return *key_analyzer_;
}

BeatAnalyzer& MusicAnalyzer::beat_analyzer() {
  if (!beat_analyzer_) {
    BeatConfig beat_config;
    beat_config.bpm_min = config_.bpm_min;
    beat_config.bpm_max = config_.bpm_max;
    beat_config.start_bpm = config_.start_bpm;
    beat_config.n_fft = config_.n_fft;
    beat_config.hop_length = config_.hop_length;
    beat_analyzer_ = std::make_unique<BeatAnalyzer>(audio_, beat_config);
  }
  return *beat_analyzer_;
}

ChordAnalyzer& MusicAnalyzer::chord_analyzer() {
  if (!chord_analyzer_) {
    ChordConfig chord_config;
    chord_config.n_fft = config_.n_fft;
    chord_config.hop_length = config_.hop_length;
    chord_analyzer_ = std::make_unique<ChordAnalyzer>(audio_, chord_config);
  }
  return *chord_analyzer_;
}

OnsetAnalyzer& MusicAnalyzer::onset_analyzer() {
  if (!onset_analyzer_) {
    OnsetDetectConfig onset_config;
    onset_config.n_fft = config_.n_fft;
    onset_config.hop_length = config_.hop_length;
    onset_analyzer_ = std::make_unique<OnsetAnalyzer>(audio_, onset_config);
  }
  return *onset_analyzer_;
}

DynamicsAnalyzer& MusicAnalyzer::dynamics_analyzer() {
  if (!dynamics_analyzer_) {
    DynamicsConfig dynamics_config;
    dynamics_config.hop_length = config_.hop_length;
    dynamics_analyzer_ = std::make_unique<DynamicsAnalyzer>(audio_, dynamics_config);
  }
  return *dynamics_analyzer_;
}

RhythmAnalyzer& MusicAnalyzer::rhythm_analyzer() {
  if (!rhythm_analyzer_) {
    RhythmConfig rhythm_config;
    rhythm_config.bpm_min = config_.bpm_min;
    rhythm_config.bpm_max = config_.bpm_max;
    rhythm_config.start_bpm = config_.start_bpm;
    rhythm_config.n_fft = config_.n_fft;
    rhythm_config.hop_length = config_.hop_length;
    rhythm_analyzer_ = std::make_unique<RhythmAnalyzer>(audio_, rhythm_config);
  }
  return *rhythm_analyzer_;
}

TimbreAnalyzer& MusicAnalyzer::timbre_analyzer() {
  if (!timbre_analyzer_) {
    TimbreConfig timbre_config;
    timbre_config.n_fft = config_.n_fft;
    timbre_config.hop_length = config_.hop_length;
    timbre_analyzer_ = std::make_unique<TimbreAnalyzer>(audio_, timbre_config);
  }
  return *timbre_analyzer_;
}

MelodyAnalyzer& MusicAnalyzer::melody_analyzer() {
  if (!melody_analyzer_) {
    MelodyConfig melody_config;
    melody_config.hop_length = config_.hop_length;
    melody_analyzer_ = std::make_unique<MelodyAnalyzer>(audio_, melody_config);
  }
  return *melody_analyzer_;
}

SectionAnalyzer& MusicAnalyzer::section_analyzer() {
  if (!section_analyzer_) {
    SectionConfig section_config;
    section_config.n_fft = config_.n_fft;
    section_config.hop_length = config_.hop_length;
    section_analyzer_ = std::make_unique<SectionAnalyzer>(audio_, section_config);
  }
  return *section_analyzer_;
}

BoundaryDetector& MusicAnalyzer::boundary_detector() {
  if (!boundary_detector_) {
    BoundaryConfig boundary_config;
    boundary_config.n_fft = config_.n_fft;
    boundary_config.hop_length = config_.hop_length;
    boundary_detector_ = std::make_unique<BoundaryDetector>(audio_, boundary_config);
  }
  return *boundary_detector_;
}

AnalysisResult MusicAnalyzer::analyze() {
  AnalysisResult result;

  // BPM and tempo (0-15%)
  report_progress(0.0f, "bpm");
  result.bpm = bpm_analyzer().bpm();
  result.bpm_confidence = bpm_analyzer().confidence();

  // Key (15-25%)
  report_progress(0.15f, "key");
  result.key = key_analyzer().key();

  // Beats and time signature (25-40%)
  report_progress(0.25f, "beats");
  result.beats = beat_analyzer().beats();
  result.time_signature = beat_analyzer().time_signature();

  // Chords (40-55%)
  report_progress(0.40f, "chords");
  result.chords = chord_analyzer().chords();

  // Sections (55-70%)
  report_progress(0.55f, "sections");
  result.sections = section_analyzer().sections();
  result.form = section_analyzer().form();

  // Timbre (70-80%)
  report_progress(0.70f, "timbre");
  result.timbre = timbre_analyzer().timbre();

  // Dynamics (80-90%)
  report_progress(0.80f, "dynamics");
  result.dynamics = dynamics_analyzer().dynamics();

  // Rhythm (90-100%)
  report_progress(0.90f, "rhythm");
  result.rhythm = rhythm_analyzer().features();

  // Complete
  report_progress(1.0f, "complete");

  return result;
}

}  // namespace sonare
