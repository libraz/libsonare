#include "analysis/music_analyzer.h"

#include "core/spectrum.h"
#include "effects/hpss.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "filters/iir.h"
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
    // Use cached onset strength to avoid recomputation
    bpm_analyzer_ = std::make_unique<BpmAnalyzer>(onset_strength(), audio_.sample_rate(),
                                                  config_.hop_length, bpm_config);
  }
  return *bpm_analyzer_;
}

KeyAnalyzer& MusicAnalyzer::key_analyzer() {
  if (!key_analyzer_) {
    KeyConfig key_config;
    key_config.hop_length = config_.hop_length;
    // Use harmonic chroma for better key detection
    key_analyzer_ = std::make_unique<KeyAnalyzer>(harmonic_chroma(), key_config);
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
    // Use cached onset strength to avoid recomputation
    beat_analyzer_ = std::make_unique<BeatAnalyzer>(onset_strength(), audio_.sample_rate(),
                                                    config_.hop_length, beat_config);
  }
  return *beat_analyzer_;
}

ChordAnalyzer& MusicAnalyzer::chord_analyzer() {
  if (!chord_analyzer_) {
    ChordConfig chord_config;
    chord_config.n_fft = config_.n_fft;
    chord_config.hop_length = config_.hop_length;
    chord_config.use_triads_only = config_.use_triads_only;
    chord_config.use_beat_sync = true;

    // Use beat-synchronized chord detection with harmonic chroma for better accuracy
    auto beat_times = beat_analyzer().beat_times();
    chord_analyzer_ = std::make_unique<ChordAnalyzer>(harmonic_chroma(), beat_times, chord_config);
  }
  return *chord_analyzer_;
}

OnsetAnalyzer& MusicAnalyzer::onset_analyzer() {
  if (!onset_analyzer_) {
    OnsetDetectConfig onset_config;
    onset_config.n_fft = config_.n_fft;
    onset_config.hop_length = config_.hop_length;
    // Use cached onset strength to avoid recomputation
    onset_analyzer_ = std::make_unique<OnsetAnalyzer>(onset_strength(), audio_.sample_rate(),
                                                      config_.hop_length, onset_config);
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
    // Use cached beat_analyzer to avoid recomputation
    rhythm_analyzer_ = std::make_unique<RhythmAnalyzer>(beat_analyzer(), rhythm_config);
  }
  return *rhythm_analyzer_;
}

TimbreAnalyzer& MusicAnalyzer::timbre_analyzer() {
  if (!timbre_analyzer_) {
    TimbreConfig timbre_config;
    timbre_config.n_fft = config_.n_fft;
    timbre_config.hop_length = config_.hop_length;
    // Use cached spectrogram and mel spectrogram to avoid recomputation
    timbre_analyzer_ =
        std::make_unique<TimbreAnalyzer>(spectrogram(), mel_spectrogram(), timbre_config);
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
    // Use cached boundary_detector's boundaries to avoid recomputation
    section_analyzer_ = std::make_unique<SectionAnalyzer>(audio_, boundary_detector().boundary_times(),
                                                          section_config);
  }
  return *section_analyzer_;
}

BoundaryDetector& MusicAnalyzer::boundary_detector() {
  if (!boundary_detector_) {
    BoundaryConfig boundary_config;
    boundary_config.n_fft = config_.n_fft;
    boundary_config.hop_length = config_.hop_length;
    // Use cached mel_spectrogram and chroma to avoid recomputation
    boundary_detector_ = std::make_unique<BoundaryDetector>(mel_spectrogram(), chroma(),
                                                            audio_.sample_rate(), boundary_config);
  }
  return *boundary_detector_;
}

const Spectrogram& MusicAnalyzer::spectrogram() {
  if (!spectrogram_) {
    StftConfig stft_config;
    stft_config.n_fft = config_.n_fft;
    stft_config.hop_length = config_.hop_length;
    spectrogram_ = std::make_unique<Spectrogram>(Spectrogram::compute(audio_, stft_config));
  }
  return *spectrogram_;
}

const Chroma& MusicAnalyzer::chroma() {
  if (!chroma_) {
    chroma_ = std::make_unique<Chroma>(Chroma::from_spectrogram(spectrogram(), audio_.sample_rate()));
  }
  return *chroma_;
}

const Chroma& MusicAnalyzer::harmonic_chroma() {
  if (!harmonic_chroma_) {
    // Optimized implementation following bpm-detector's key detection algorithm:
    // 1. HPSS to extract harmonic content (margin=3.0)
    // 2. High-pass filter (80Hz, 4th order Butterworth)
    // 3. CQT-based chroma extraction
    // 4. Bass-weighted chroma combination (50% full + 50% bass)
    //
    // WASM optimizations:
    // - Larger hop_length for CQT (chroma_hop_multiplier)
    // - Optional bass-weighted computation (use_bass_weighted)
    // - Reduced CQT bins for bass (36 bins = 3 octaves)

    int sr = audio_.sample_rate();
    int chroma_hop = config_.hop_length * config_.chroma_hop_multiplier;

    // Step 1: Apply HPSS once with margin=3.0 to extract harmonic content
    Audio harmonic_audio = audio_;
    if (config_.use_hpss) {
      HpssConfig hpss_config;
      hpss_config.margin_harmonic = 3.0f;
      hpss_config.margin_percussive = 3.0f;

      StftConfig stft_config;
      stft_config.n_fft = config_.n_fft;
      stft_config.hop_length = config_.hop_length;

      harmonic_audio = harmonic(audio_, hpss_config, stft_config);
    }

    // Step 2: Apply 4th order Butterworth high-pass filter
    Audio filtered_audio = harmonic_audio;
    if (config_.chroma_highpass_hz > 0) {
      auto cascade = highpass_coeffs_4th(config_.chroma_highpass_hz, sr);
      auto filtered = apply_cascade_filtfilt(harmonic_audio.data(), harmonic_audio.size(), cascade);
      filtered_audio = Audio::from_vector(std::move(filtered), sr);
    }

    // Step 3: Compute CQT-based chroma with larger hop for speed
    CqtConfig cqt_config;
    cqt_config.hop_length = chroma_hop;
    cqt_config.fmin = 32.7f;  // C1
    cqt_config.n_bins = 84;   // 7 octaves * 12 bins
    cqt_config.bins_per_octave = 12;

    CqtResult cqt_result = cqt(filtered_audio, cqt_config);
    std::vector<float> chroma_features = cqt_to_chroma(cqt_result, 12);
    int n_frames = cqt_result.n_frames();

    // Step 4: Bass-weighted chroma (optional, controlled by use_bass_weighted)
    if (config_.use_bass_weighted && n_frames > 0) {
      // Apply preemphasis to filtered audio (already harmonic)
      auto preemph_samples = preemphasis(filtered_audio.data(), filtered_audio.size(), 0.97f);

      // Use reduced CQT for bass (lower 4 octaves = 48 bins)
      CqtConfig bass_cqt_config;
      bass_cqt_config.hop_length = chroma_hop;
      bass_cqt_config.fmin = 32.7f;
      bass_cqt_config.n_bins = 48;  // 4 octaves (bass-focused)
      bass_cqt_config.bins_per_octave = 12;

      Audio preemph_audio = Audio::from_vector(std::move(preemph_samples), sr);
      CqtResult bass_cqt = cqt(preemph_audio, bass_cqt_config);
      std::vector<float> bass_chroma = cqt_to_chroma(bass_cqt, 12);

      // Combine: 50% full-range + 50% bass-weighted
      int bass_frames = bass_cqt.n_frames();
      int min_frames = std::min(n_frames, bass_frames);

      for (int c = 0; c < 12; ++c) {
        for (int t = 0; t < min_frames; ++t) {
          int idx = c * n_frames + t;
          int bass_idx = c * bass_frames + t;
          chroma_features[idx] = 0.5f * chroma_features[idx] + 0.5f * bass_chroma[bass_idx];
        }
      }
    }

    harmonic_chroma_ =
        std::make_unique<Chroma>(Chroma(std::move(chroma_features), 12, n_frames, sr, chroma_hop));
  }
  return *harmonic_chroma_;
}

const MelSpectrogram& MusicAnalyzer::mel_spectrogram() {
  if (!mel_spectrogram_) {
    MelFilterConfig mel_filter_config;
    mel_filter_config.n_mels = 128;
    mel_spectrogram_ = std::make_unique<MelSpectrogram>(
        MelSpectrogram::from_spectrogram(spectrogram(), audio_.sample_rate(), mel_filter_config));
  }
  return *mel_spectrogram_;
}

const std::vector<float>& MusicAnalyzer::onset_strength() {
  if (!onset_strength_computed_) {
    OnsetConfig onset_config;
    onset_strength_ = compute_onset_strength(mel_spectrogram(), onset_config);
    onset_strength_computed_ = true;
  }
  return onset_strength_;
}

AnalysisResult MusicAnalyzer::analyze() {
  AnalysisResult result;

  // BPM and tempo (0-15%)
  report_progress(0.0f, "bpm");
  result.bpm = bpm_analyzer().bpm();
  result.bpm_confidence = bpm_analyzer().confidence();

  // Key (15-25%) - initial detection from chroma
  report_progress(0.15f, "key");
  Key chroma_key = key_analyzer().key();

  // Beats and time signature (25-40%)
  report_progress(0.25f, "beats");
  result.beats = beat_analyzer().beats();
  result.time_signature = beat_analyzer().time_signature();

  // Chords (40-55%)
  report_progress(0.40f, "chords");
  result.chords = chord_analyzer().chords();

  // Refine key using chord progression analysis
  result.key = refine_key_with_chords(chroma_key, result.chords);

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
