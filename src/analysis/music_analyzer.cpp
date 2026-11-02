#include "analysis/music_analyzer.h"

#include "analysis/analysis_rate.h"
#include "analysis/downbeat_analyzer.h"
#include "analysis/section_structure.h"
#include "analysis/tempo_curve.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

#ifndef __EMSCRIPTEN__
#include <future>
#endif

namespace sonare {

void validate_config(const MusicAnalyzerConfig& config) {
  // A real-valued FFT of an odd size has no n_fft/2 + 1 bin layout, so an odd
  // size is rejected rather than silently rounded.
  SONARE_CHECK_MSG(config.n_fft >= 2 && (config.n_fft & 1) == 0, ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: nFft must be an even value of at least 2");
  SONARE_CHECK_MSG(config.hop_length > 0, ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: hopLength must be positive");
  SONARE_CHECK_MSG(numeric::finite_positive(config.bpm_min), ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: bpmMin must be finite and positive");
  SONARE_CHECK_MSG(numeric::finite(config.bpm_max) && config.bpm_max >= config.bpm_min,
                   ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: bpmMax must be finite and at least bpmMin");
  SONARE_CHECK_MSG(numeric::finite_positive(config.start_bpm), ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: startBpm must be finite and positive");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.chroma_highpass_hz),
                   ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: chromaHighpassHz must be finite and non-negative");
  SONARE_CHECK_MSG(config.chroma_hop_multiplier > 0, ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: chromaHopMultiplier must be positive");
  SONARE_CHECK_MSG(!config.tuning || is_valid_chroma_tuning(*config.tuning),
                   ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: tuning must be finite and in [-0.5, 0.5)");
  // A non-positive beam width disables the HMM beam cut instead of erroring,
  // which turns chord smoothing into an order-of-magnitude slower full search.
  SONARE_CHECK_MSG(config.chord_hmm_beam_width > 0, ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: chordHmmBeamWidth must be positive");
  SONARE_CHECK_MSG(config.tempo_update_interval_beats > 0, ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: tempoUpdateIntervalBeats must be positive");
  // An empty candidate set makes the meter estimator return a fixed
  // low-confidence 4/4 rather than searching, which reads as a detection
  // result. Reject it instead so a caller who cleared the list is told.
  SONARE_CHECK_MSG(!config.meter_candidate_numerators.empty(), ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: meterCandidateNumerators must not be empty");
  SONARE_CHECK_MSG(
      config.meter_candidate_numerators.size() <= static_cast<size_t>(kMaxMeterCandidateNumerators),
      ErrorCode::InvalidParameter,
      "MusicAnalyzerConfig: meterCandidateNumerators must hold at most 16 entries");
  for (int numerator : config.meter_candidate_numerators) {
    SONARE_CHECK_MSG(
        numerator >= kMinMeterCandidateNumerator && numerator <= kMaxMeterCandidateNumerator,
        ErrorCode::InvalidParameter,
        "MusicAnalyzerConfig: meterCandidateNumerators entries must be in [2, 32]");
  }
  // Only a power of two is a note value, and the estimator reports 8 itself
  // when it resolves a compound meter, so a non-power-of-two could never
  // round-trip through the reported signature. The ceiling is a separate
  // question, on kMaxMeterDenominator.
  SONARE_CHECK_MSG(config.meter_denominator > 0 &&
                       config.meter_denominator <= kMaxMeterDenominator &&
                       (config.meter_denominator & (config.meter_denominator - 1)) == 0,
                   ErrorCode::InvalidParameter,
                   "MusicAnalyzerConfig: meterDenominator must be a power of two in [1, 32]");
}

MusicAnalyzer::MusicAnalyzer(const Audio& audio, const MusicAnalyzerConfig& config)
    : audio_(audio),
      config_(Validated<MusicAnalyzerConfig>::make(config)),
      progress_callback_(nullptr) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Downsample to 22050 Hz for spectral analysis if sample rate is higher
  analysis_audio_ = analysis_rate_audio(audio_);
  analysis_sr_ = analysis_audio_.sample_rate();
  // A lower input keeps its rate, so the window is rescaled to it.
  analysis_stft_ = stft_config_at_rate(config_->n_fft, config_->hop_length, analysis_sr_);
  // Resolved before any prefetch, so the chord inputs and the key read one value.
  tuning_ = config_->tuning ? *config_->tuning : measure_tuning(analysis_audio_);
}

void MusicAnalyzer::set_progress_callback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
}

void MusicAnalyzer::set_cancel_callback(CancelCallback should_cancel) {
  cancel_callback_ = std::move(should_cancel);
}

void MusicAnalyzer::report_progress(float progress, const char* stage) {
#ifndef __EMSCRIPTEN__
  std::lock_guard<std::mutex> lock(progress_mutex_);
#endif
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
  std::call_once(bpm_analyzer_once_, [this]() {
    BpmConfig bpm_config;
    bpm_config.bpm_min = config_->bpm_min;
    bpm_config.bpm_max = config_->bpm_max;
    bpm_config.start_bpm = config_->start_bpm;
    bpm_config.n_fft = config_->n_fft;
    bpm_config.hop_length = config_->hop_length;
    // Use cached onset strength to avoid recomputation
    bpm_analyzer_ = std::make_unique<BpmAnalyzer>(onset_strength(), analysis_sr_,
                                                  config_->hop_length, bpm_config);
  });
  return *bpm_analyzer_;
}

KeyAnalyzer& MusicAnalyzer::key_analyzer() {
  std::call_once(key_analyzer_once_, [this]() {
    // The key reads the chord front end without its bass blend, as detect_key does.
    KeyConfig key_config;
    key_config.n_fft = config_->n_fft;
    key_config.hop_length = config_->hop_length;
    key_config.use_hpss = config_->use_hpss;
    key_config.high_pass_hz = config_->chroma_highpass_hz;
    key_config.tuning = tuning_;
    key_analyzer_ = std::make_unique<KeyAnalyzer>(analysis_audio_, key_config);
  });
  return *key_analyzer_;
}

BeatAnalyzer& MusicAnalyzer::beat_analyzer() {
  std::call_once(beat_analyzer_once_, [this]() {
    BeatConfig beat_config;
    beat_config.bpm_min = config_->bpm_min;
    beat_config.bpm_max = config_->bpm_max;
    beat_config.start_bpm = config_->start_bpm;
    beat_config.n_fft = config_->n_fft;
    beat_config.hop_length = config_->hop_length;
    beat_config.adaptive_tempo = config_->adaptive_tempo;
    beat_config.tempo_update_interval_beats = config_->tempo_update_interval_beats;
    beat_config.meter_candidate_numerators = config_->meter_candidate_numerators;
    beat_config.meter_denominator = config_->meter_denominator;
    // Use cached onset strength to avoid recomputation
    beat_analyzer_ = std::make_unique<BeatAnalyzer>(
        onset_strength(), analysis_sr_, config_->hop_length, beat_config,
        onset_center_shift_frames(analysis_stft_, true));
    // Same order as the Audio constructor: the meter reads the energy, then the bar phase is
    // selected from the cached analysis spectrogram.
    beat_analyzer_->estimate_time_signature(beat_low_frequency_energy());
    const Spectrogram& spec = spectrogram();
    beat_analyzer_->refine_downbeats(downbeat_phase_evidence(
        spec.power(), spec.n_bins(), spec.n_frames(), analysis_sr_, analysis_stft_.n_fft,
        config_->hop_length, beat_analyzer_->beats()));
  });
  return *beat_analyzer_;
}

ChordAnalyzer& MusicAnalyzer::chord_analyzer() {
  std::call_once(chord_analyzer_once_, [this]() {
    ChordConfig chord_config;
    chord_config.n_fft = config_->n_fft;
    chord_config.hop_length = config_->hop_length;
    chord_config.use_triads_only = config_->use_triads_only;
    chord_config.use_beat_sync = true;
    chord_config.use_hmm = config_->use_chord_hmm;
    chord_config.hmm_beam_width = config_->chord_hmm_beam_width;
    chord_config.detect_inversions = config_->detect_chord_inversions;
    chord_config.tuning = tuning_;
    if (config_->use_chord_key_context) {
      const Key key = key_analyzer().key();
      chord_config.use_key_context = true;
      chord_config.key_root = key.root;
      chord_config.key_mode = key.mode;
    }

    // Chord windows and chord-change matching both read the reported beat times.
    BeatAnalyzer& beats = beat_analyzer();
    chord_inputs();
    chord_analyzer_ = std::make_unique<ChordAnalyzer>(*chord_chroma_, beats.beat_times(),
                                                      *chord_bass_chroma_, chord_config);
    beats.set_chord_change_observations(
        chord_change_observations(beats.beats(), chord_analyzer_->chords()));
  });
  return *chord_analyzer_;
}

OnsetAnalyzer& MusicAnalyzer::onset_analyzer() {
  std::call_once(onset_analyzer_once_, [this]() {
    OnsetDetectConfig onset_config;
    onset_config.n_fft = config_->n_fft;
    onset_config.hop_length = config_->hop_length;
    // Use cached onset strength to avoid recomputation
    onset_analyzer_ = std::make_unique<OnsetAnalyzer>(
        onset_strength(), analysis_sr_, config_->hop_length, onset_config,
        onset_center_shift_frames(analysis_stft_, true));
  });
  return *onset_analyzer_;
}

DynamicsAnalyzer& MusicAnalyzer::dynamics_analyzer() {
  std::call_once(dynamics_analyzer_once_, [this]() {
    DynamicsConfig dynamics_config;
    // Levels read the native audio; the hop is samples at the analysis rate, so it is rescaled.
    dynamics_config.hop_length =
        window_length_at_rate(config_->hop_length, audio_.sample_rate(), analysis_sr_);
    dynamics_analyzer_ = std::make_unique<DynamicsAnalyzer>(audio_, dynamics_config);
  });
  return *dynamics_analyzer_;
}

RhythmAnalyzer& MusicAnalyzer::rhythm_analyzer() {
  std::call_once(rhythm_analyzer_once_, [this]() {
    RhythmConfig rhythm_config;
    rhythm_config.bpm_min = config_->bpm_min;
    rhythm_config.bpm_max = config_->bpm_max;
    rhythm_config.start_bpm = config_->start_bpm;
    rhythm_config.n_fft = config_->n_fft;
    rhythm_config.hop_length = config_->hop_length;
    rhythm_config.meter_candidate_numerators = config_->meter_candidate_numerators;
    rhythm_config.meter_denominator = config_->meter_denominator;
    // Use cached beat_analyzer to avoid recomputation
    rhythm_analyzer_ = std::make_unique<RhythmAnalyzer>(beat_analyzer(), rhythm_config);
  });
  return *rhythm_analyzer_;
}

TimbreAnalyzer& MusicAnalyzer::timbre_analyzer() {
  std::call_once(timbre_analyzer_once_, [this]() {
    TimbreConfig timbre_config;
    timbre_config.n_fft = config_->n_fft;
    timbre_config.hop_length = config_->hop_length;
    // Use cached spectrogram and mel spectrogram to avoid recomputation
    timbre_analyzer_ =
        std::make_unique<TimbreAnalyzer>(spectrogram(), mel_spectrogram(), timbre_config);
  });
  return *timbre_analyzer_;
}

MelodyAnalyzer& MusicAnalyzer::melody_analyzer() {
  std::call_once(melody_analyzer_once_, [this]() {
    MelodyConfig melody_config;
    melody_config.hop_length = config_->hop_length;
    melody_analyzer_ = std::make_unique<MelodyAnalyzer>(analysis_audio_, melody_config);
  });
  return *melody_analyzer_;
}

SectionAnalyzer& MusicAnalyzer::section_analyzer() {
  std::call_once(section_analyzer_once_, [this]() {
    SectionConfig section_config;
    section_config.n_fft = config_->n_fft;
    section_config.hop_length = config_->hop_length;
    section_config.tuning = tuning_;
    // The cached spectrogram is the analysis-rate signal's, the one the sections are read off.
    section_analyzer_ = std::make_unique<SectionAnalyzer>(
        analysis_audio_, section_structure::make_section_grid(beat_analyzer()), spectrogram(),
        section_config);
  });
  return *section_analyzer_;
}

BoundaryDetector& MusicAnalyzer::boundary_detector() {
  std::call_once(boundary_detector_once_, [this]() {
    BoundaryConfig boundary_config;
    boundary_config.n_fft = config_->n_fft;
    boundary_config.hop_length = config_->hop_length;
    // Use cached mel_spectrogram and chroma to avoid recomputation
    boundary_detector_ = std::make_unique<BoundaryDetector>(mel_spectrogram(), chroma(),
                                                            analysis_sr_, boundary_config);
  });
  return *boundary_detector_;
}

const Spectrogram& MusicAnalyzer::spectrogram() {
  std::call_once(spectrogram_once_, [this]() {
    spectrogram_ =
        std::make_unique<Spectrogram>(Spectrogram::compute(analysis_audio_, analysis_stft_));
  });
  return *spectrogram_;
}

const Chroma& MusicAnalyzer::chroma() {
  std::call_once(chroma_once_, [this]() {
    ChromaFilterConfig filter_config;
    filter_config.tuning = tuning_;
    chroma_ = std::make_unique<Chroma>(
        Chroma::from_spectrogram(spectrogram(), analysis_sr_, filter_config));
  });
  return *chroma_;
}

void MusicAnalyzer::chord_inputs() {
  std::call_once(chord_inputs_once_, [this]() {
    ChordChromaConfig chroma_config;
    chroma_config.hop_length = config_->hop_length * config_->chroma_hop_multiplier;
    chroma_config.tuning = tuning_;
    chroma_config.use_hpss = config_->use_hpss;
    chroma_config.hpss_n_fft = config_->n_fft;
    chroma_config.hpss_hop_length = config_->hop_length;
    chroma_config.highpass_hz = config_->chroma_highpass_hz;
    chroma_config.bass_weighted = config_->use_bass_weighted;
    chord_chroma_ = std::make_unique<Chroma>(chord_chroma(analysis_audio_, chroma_config));

    BassChromaConfig bass_config;
    bass_config.cqt.hop_length = config_->hop_length;
    bass_config.cqt.fmin = tune_cqt_fmin(bass_config.cqt.fmin, tuning_);
    chord_bass_chroma_ = std::make_unique<Chroma>(bass_chroma(analysis_audio_, bass_config));
  });
}

const MelSpectrogram& MusicAnalyzer::mel_spectrogram() {
  std::call_once(mel_spectrogram_once_, [this]() {
    MelFilterConfig mel_filter_config;
    mel_filter_config.n_mels = constants::kDefaultNMels;
    mel_spectrogram_ = std::make_unique<MelSpectrogram>(
        MelSpectrogram::from_spectrogram(spectrogram(), analysis_sr_, mel_filter_config));
  });
  return *mel_spectrogram_;
}

const std::vector<float>& MusicAnalyzer::onset_strength() {
  std::call_once(onset_strength_once_, [this]() {
    OnsetConfig onset_config;
    // Internal analysis pipeline opts into detrend explicitly (the public
    // OnsetConfig default is detrend=false to match librosa).
    onset_config.detrend = true;
    onset_strength_ = center_onset_strength(compute_onset_strength(mel_spectrogram(), onset_config),
                                            analysis_stft_, onset_config.center);
  });
  return onset_strength_;
}

const std::vector<float>& MusicAnalyzer::beat_low_frequency_energy() {
  std::call_once(beat_low_frequency_energy_once_, [this]() {
    beat_low_frequency_energy_ =
        low_frequency_energy_observations(beat_analyzer_->beats(), analysis_audio_);
  });
  return beat_low_frequency_energy_;
}

void MusicAnalyzer::precompute_features() {
#ifndef __EMSCRIPTEN__
  // Native: compute the two independent heavy paths in parallel
  // Group A: spectrogram → mel_spectrogram → onset_strength
  auto future_a = std::async(std::launch::async, [this]() {
    spectrogram();
    mel_spectrogram();
    onset_strength();
  });

  // Group B: chord chroma and bass chroma (constant-Q)
  auto future_b = std::async(std::launch::async, [this]() { chord_inputs(); });

  future_a.get();
  future_b.get();
#else
  // WASM: sequential execution (no threading support)
  spectrogram();
  mel_spectrogram();
  onset_strength();
  chord_inputs();
#endif
}

template <bool CheckCancel>
std::optional<AnalysisResult> MusicAnalyzer::analyze_impl() {
  AnalysisResult result;

  // Eagerly precompute feature caches (parallel on native, sequential on WASM)
  report_progress(0.0f, "features");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  precompute_features();

  // BPM and tempo
  report_progress(0.15f, "bpm");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.bpm = bpm_analyzer().bpm();
  result.bpm_confidence = bpm_analyzer().confidence();
  result.bpm_candidates.push_back(
      {result.bpm, result.bpm_confidence, BpmCandidateRelation::Primary});
  for (const BpmCandidate& candidate : bpm_analyzer().candidates()) {
    const float distance = std::abs(candidate.bpm - result.bpm);
    const float primary_scale = std::max(1.0f, std::abs(result.bpm));
    if (distance <= primary_scale * 0.01f) continue;

    BpmCandidateRelation relation = BpmCandidateRelation::Other;
    if (std::abs(candidate.bpm * 2.0f - result.bpm) <= primary_scale * 0.03f) {
      relation = BpmCandidateRelation::Half;
    } else if (std::abs(candidate.bpm - result.bpm * 2.0f) <= primary_scale * 0.06f) {
      relation = BpmCandidateRelation::Double;
    }
    result.bpm_candidates.push_back({candidate.bpm, candidate.confidence, relation});
  }

  // Key (15-25%) - initial detection from chroma
  report_progress(0.15f, "key");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  const std::vector<KeyCandidate>& chroma_candidates = key_analyzer().all_candidates();

  // Beats and time signature (25-40%)
  report_progress(0.25f, "beats");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.beats = beat_analyzer().beats();
  result.time_signature = beat_analyzer().time_signature();
  result.time_signature_candidates = beat_analyzer().time_signature_candidates();
  result.downbeat_indices = beat_analyzer().downbeat_indices();
  result.downbeat_phase = beat_analyzer().downbeat_phase();

  // Chords (40-55%)
  report_progress(0.40f, "chords");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.chords = chord_analyzer().chords();

  // Read after chord_analyzer(), which publishes the chord-change stream on the beat analyzer.
  result.beat_observations.onset_strength = beat_analyzer().beat_onset_observations();
  result.beat_observations.low_frequency_energy = beat_analyzer().beat_low_frequency_observations();
  result.beat_observations.chord_change = beat_analyzer().beat_chord_change_observations();

  if (config_->compute_tempo_curve) {
    // Decoded from the same beat grid published above, and by the same decoder
    // the arrangement tempo bridge runs, so a curve read here and the tempo
    // segments written into a project describe one estimate rather than two.
    result.beat_local_bpm =
        estimate_beat_local_bpm(result.beats, beat_analyzer().onset_strength(),
                                beat_analyzer().sample_rate(), beat_analyzer().hop_length());
  }

  // Refine key using chord progression analysis
  result.key = refine_key_with_chords(chroma_candidates, result.chords);
  // Whichever key the chords settle on, it reports its chroma posterior like detect_key does.
  result.key.confidence = key_posterior(key_analyzer().all_candidates(), result.key);
  result.chord_roman_numerals.reserve(result.chords.size());
  for (const Chord& chord : result.chords) {
    result.chord_roman_numerals.push_back(
        chord.quality == ChordQuality::Unknown
            ? std::string()
            : ChordAnalyzer::chord_to_roman_numeral(chord, result.key.root, result.key.mode));
  }

  // Sections (55-70%)
  report_progress(0.55f, "sections");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.sections = section_analyzer().sections();
  result.form = section_analyzer().form();
  result.tuning = tuning_;

  // Timbre (70-80%)
  report_progress(0.70f, "timbre");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.timbre = timbre_analyzer().timbre();

  // Dynamics (80-90%)
  report_progress(0.80f, "dynamics");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.dynamics = dynamics_analyzer().dynamics();

  // Rhythm (90-95%)
  report_progress(0.90f, "rhythm");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.rhythm = rhythm_analyzer().features();

  // Melody / pitch contour (95-100%)
  report_progress(0.95f, "melody");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }
  result.melody = melody_analyzer().contour();

  // Complete
  report_progress(1.0f, "complete");
  if constexpr (CheckCancel) {
    if (cancel_callback_ && cancel_callback_()) return std::nullopt;
  }

  return result;
}

AnalysisResult MusicAnalyzer::analyze() { return *analyze_impl<false>(); }

std::optional<AnalysisResult> MusicAnalyzer::analyze_cancellable() { return analyze_impl<true>(); }

}  // namespace sonare
