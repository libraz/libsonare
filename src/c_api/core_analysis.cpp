#include "c_api/core_internal.h"

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, int hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0 ||
      max_candidates < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->candidates = nullptr;
  out->candidate_count = 0;
  out->autocorrelation = nullptr;
  out->autocorrelation_count = 0;
  out->tempogram = nullptr;
  out->tempogram_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    BpmConfig config;
    config.bpm_min = bpm_min;
    config.bpm_max = bpm_max;
    config.start_bpm = start_bpm;
    config.n_fft = n_fft;
    config.hop_length = hop_length;

    BpmAnalyzer analyzer(audio, config);
    out->bpm = analyzer.bpm();
    out->confidence = analyzer.confidence();

    std::vector<BpmCandidate> candidates = analyzer.candidates(max_candidates);
    out->candidate_count = candidates.size();
    if (!candidates.empty()) {
      std::unique_ptr<SonareBpmCandidate[]> cands(new SonareBpmCandidate[candidates.size()]);
      for (size_t i = 0; i < candidates.size(); ++i) {
        cands[i].bpm = candidates[i].bpm;
        cands[i].confidence = candidates[i].confidence;
      }
      out->candidates = release_array(cands);
    }

    const std::vector<float>& autocorr = analyzer.autocorrelation();
    out->autocorrelation_count = autocorr.size();
    if (!autocorr.empty()) {
      std::unique_ptr<float[]> data(new float[autocorr.size()]);
      std::memcpy(data.get(), autocorr.data(), autocorr.size() * sizeof(float));
      out->autocorrelation = release_array(data);
    }

    const std::vector<float>& tempogram = analyzer.tempogram();
    out->tempogram_count = tempogram.size();
    if (!tempogram.empty()) {
      std::unique_ptr<float[]> data(new float[tempogram.size()]);
      std::memcpy(data.get(), tempogram.data(), tempogram.size() * sizeof(float));
      out->tempogram = release_array(data);
    }
    return SONARE_OK;
  });
}

SonareError sonare_analyze_impulse_response(const float* samples, size_t length, int sample_rate,
                                            int n_octave_bands, SonareAcousticResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_octave_bands < 0) return SONARE_ERROR_INVALID_PARAMETER;

  // Zero the whole struct up front so a rejected input (validate_audio_params
  // failure) leaves a fully-initialized result: the scalar fields
  // (rt60/edt/c50/c80/d50/confidence/is_blind) as well as the band pointers /
  // band_count are all defined, not caller garbage.
  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AcousticConfig config;
    config.n_octave_bands = n_octave_bands;
    fill_acoustic_result(sonare::analyze_impulse_response(audio, config), out);
    return SONARE_OK;
  });
}

SonareError sonare_detect_acoustic(const float* samples, size_t length, int sample_rate,
                                   int n_octave_bands, int n_third_octave_subbands,
                                   float min_decay_db, float noise_floor_margin_db,
                                   SonareAcousticResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_octave_bands < 0 || n_third_octave_subbands < 0 || min_decay_db <= 0.0f ||
      noise_floor_margin_db < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  // Zero the whole struct up front so a rejected input (validate_audio_params
  // failure) leaves a fully-initialized result: the scalar fields
  // (rt60/edt/c50/c80/d50/confidence/is_blind) as well as the band pointers /
  // band_count are all defined, not caller garbage.
  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    AcousticConfig config;
    config.mode = AcousticConfig::Mode::Blind;
    config.n_octave_bands = n_octave_bands;
    config.n_third_octave_subbands = n_third_octave_subbands;
    config.min_decay_db = min_decay_db;
    config.noise_floor_margin_db = noise_floor_margin_db;
    fill_acoustic_result(sonare::detect_acoustic(audio, config), out);
    return SONARE_OK;
  });
}

SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length, SonareRhythmResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (bpm_min <= 0.0f || bpm_max <= bpm_min || n_fft <= 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->beat_intervals = nullptr;
  out->beat_interval_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    RhythmConfig config;
    config.bpm_min = bpm_min;
    config.bpm_max = bpm_max;
    config.start_bpm = start_bpm;
    config.n_fft = n_fft;
    config.hop_length = hop_length;

    RhythmAnalyzer analyzer(audio, config);
    RhythmFeatures features = analyzer.features();
    out->bpm = analyzer.bpm();
    out->time_signature.numerator = features.time_signature.numerator;
    out->time_signature.denominator = features.time_signature.denominator;
    out->time_signature.confidence = features.time_signature.confidence;
    out->groove_type = to_c_groove_type(features.groove_type);
    out->syncopation = features.syncopation;
    out->pattern_regularity = features.pattern_regularity;
    out->tempo_stability = features.tempo_stability;

    const std::vector<float>& intervals = analyzer.beat_intervals();
    out->beat_interval_count = intervals.size();
    if (!intervals.empty()) {
      std::unique_ptr<float[]> data(new float[intervals.size()]);
      std::memcpy(data.get(), intervals.data(), intervals.size() * sizeof(float));
      out->beat_intervals = release_array(data);
    }
    return SONARE_OK;
  });
}

SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, int hop_length, float compression_threshold,
                                    SonareDynamicsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (window_sec <= 0.0f || hop_length <= 0 || compression_threshold < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->loudness_times = nullptr;
  out->loudness_rms_db = nullptr;
  out->loudness_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    DynamicsConfig config;
    config.window_sec = window_sec;
    config.hop_length = hop_length;
    config.compression_threshold = compression_threshold;

    DynamicsAnalyzer analyzer(audio, config);
    const Dynamics& dynamics = analyzer.dynamics();
    out->dynamic_range_db = dynamics.dynamic_range_db;
    out->peak_db = dynamics.peak_db;
    out->rms_db = dynamics.rms_db;
    out->crest_factor = dynamics.crest_factor;
    out->loudness_range_db = dynamics.loudness_range_db;
    out->is_compressed = dynamics.is_compressed ? 1 : 0;

    const LoudnessCurve& curve = analyzer.loudness_curve();
    size_t count = std::min(curve.times.size(), curve.rms_db.size());
    out->loudness_count = count;
    if (count > 0) {
      std::unique_ptr<float[]> times(new float[count]);
      std::unique_ptr<float[]> rms_db(new float[count]);
      std::memcpy(times.get(), curve.times.data(), count * sizeof(float));
      std::memcpy(rms_db.get(), curve.rms_db.data(), count * sizeof(float));
      out->loudness_times = release_array(times);
      out->loudness_rms_db = release_array(rms_db);
    }
    return SONARE_OK;
  });
}

SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || n_mels <= 0 || n_mfcc <= 0 || window_sec <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->spectral_centroid = nullptr;
  out->spectral_centroid_count = 0;
  out->spectral_flatness = nullptr;
  out->spectral_flatness_count = 0;
  out->spectral_rolloff = nullptr;
  out->spectral_rolloff_count = 0;
  out->timbre_over_time = nullptr;
  out->timbre_over_time_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    TimbreConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    config.n_mfcc = n_mfcc;
    config.window_sec = window_sec;

    TimbreAnalyzer analyzer(audio, config);
    const Timbre& timbre = analyzer.timbre();
    out->brightness = timbre.brightness;
    out->warmth = timbre.warmth;
    out->density = timbre.density;
    out->roughness = timbre.roughness;
    out->complexity = timbre.complexity;

    const std::vector<float>& centroid = analyzer.spectral_centroid();
    out->spectral_centroid_count = centroid.size();
    if (!centroid.empty()) {
      std::unique_ptr<float[]> data(new float[centroid.size()]);
      std::memcpy(data.get(), centroid.data(), centroid.size() * sizeof(float));
      out->spectral_centroid = release_array(data);
    }

    const std::vector<float>& flatness = analyzer.spectral_flatness();
    out->spectral_flatness_count = flatness.size();
    if (!flatness.empty()) {
      std::unique_ptr<float[]> data(new float[flatness.size()]);
      std::memcpy(data.get(), flatness.data(), flatness.size() * sizeof(float));
      out->spectral_flatness = release_array(data);
    }

    const std::vector<float>& rolloff = analyzer.spectral_rolloff();
    out->spectral_rolloff_count = rolloff.size();
    if (!rolloff.empty()) {
      std::unique_ptr<float[]> data(new float[rolloff.size()]);
      std::memcpy(data.get(), rolloff.data(), rolloff.size() * sizeof(float));
      out->spectral_rolloff = release_array(data);
    }

    const std::vector<Timbre>& over_time = analyzer.timbre_over_time();
    out->timbre_over_time_count = over_time.size();
    if (!over_time.empty()) {
      std::unique_ptr<SonareTimbreFrame[]> frames(new SonareTimbreFrame[over_time.size()]);
      for (size_t i = 0; i < over_time.size(); ++i) {
        frames[i].brightness = over_time[i].brightness;
        frames[i].warmth = over_time[i].warmth;
        frames[i].density = over_time[i].density;
        frames[i].roughness = over_time[i].roughness;
        frames[i].complexity = over_time[i].complexity;
      }
      out->timbre_over_time = release_array(frames);
    }
    return SONARE_OK;
  });
}

SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 int use_triads_only, int n_fft, int hop_length, int use_beat_sync,
                                 SonareChordAnalysisResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (min_duration < 0.0f || smoothing_window <= 0.0f || threshold < 0.0f || n_fft <= 0 ||
      hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->chords = nullptr;
  out->chord_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChordConfig config;
    config.min_duration = min_duration;
    config.smoothing_window = smoothing_window;
    config.threshold = threshold;
    config.use_triads_only = use_triads_only != 0;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.use_beat_sync = use_beat_sync != 0;

    std::vector<Chord> chords = detect_chords(audio, config);
    fill_chord_result(chords, out);
    return SONARE_OK;
  });
}

SonareError sonare_detect_chords_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareChordDetectionOptions* options,
                                    SonareChordAnalysisResult* out) {
  if (!out || !options) return SONARE_ERROR_INVALID_PARAMETER;
  if (options->min_duration < 0.0f || options->smoothing_window <= 0.0f ||
      options->threshold < 0.0f || options->n_fft <= 0 || options->hop_length <= 0 ||
      options->hmm_beam_width < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Reject out-of-range enum-like fields instead of silently mapping them to a
  // default (chroma_method only documents 0 = STFT, 1 = NNLS). When key context
  // is enabled, also require an in-range key_root / key_mode rather than letting
  // from_c_pitch_class / from_c_mode silently clamp garbage to C / Major.
  if (options->chroma_method != 0 && options->chroma_method != 1) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (options->use_key_context != 0) {
    const int root = static_cast<int>(options->key_root);
    const int mode = static_cast<int>(options->key_mode);
    if (root < static_cast<int>(PitchClass::C) || root > static_cast<int>(PitchClass::B) ||
        mode < static_cast<int>(Mode::Major) || mode > static_cast<int>(Mode::Locrian)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  out->chords = nullptr;
  out->chord_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChordConfig config;
    config.min_duration = options->min_duration;
    config.smoothing_window = options->smoothing_window;
    config.threshold = options->threshold;
    config.use_triads_only = options->use_triads_only != 0;
    config.n_fft = options->n_fft;
    config.hop_length = options->hop_length;
    config.use_beat_sync = options->use_beat_sync != 0;
    config.use_hmm = options->use_hmm != 0;
    config.hmm_beam_width = options->hmm_beam_width;
    config.use_key_context = options->use_key_context != 0;
    config.key_root = from_c_pitch_class(options->key_root);
    config.key_mode = from_c_mode(options->key_mode);
    config.detect_inversions = options->detect_inversions != 0;
    config.chroma_method = options->chroma_method == 1 ? ChromaMethod::NNLS : ChromaMethod::STFT;

    std::vector<Chord> chords = detect_chords(audio, config);
    fill_chord_result(chords, out);
    return SONARE_OK;
  });
}

SonareError sonare_chord_functional_analysis(const float* samples, size_t length, int sample_rate,
                                             const SonareChordDetectionOptions* options,
                                             SonarePitchClass key_root, SonareMode key_mode,
                                             SonareStringArray* out) {
  if (!out || !options) return SONARE_ERROR_INVALID_PARAMETER;
  if (options->min_duration < 0.0f || options->smoothing_window <= 0.0f ||
      options->threshold < 0.0f || options->n_fft <= 0 || options->hop_length <= 0 ||
      options->hmm_beam_width < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (options->chroma_method != 0 && options->chroma_method != 1) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (options->use_key_context != 0) {
    const int root = static_cast<int>(options->key_root);
    const int mode = static_cast<int>(options->key_mode);
    if (root < static_cast<int>(PitchClass::C) || root > static_cast<int>(PitchClass::B) ||
        mode < static_cast<int>(Mode::Major) || mode > static_cast<int>(Mode::Locrian)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  // The analysis key drives the Roman-numeral labelling; reject out-of-range
  // enum-like values rather than silently clamping them to C / Major.
  {
    const int root = static_cast<int>(key_root);
    const int mode = static_cast<int>(key_mode);
    if (root < static_cast<int>(PitchClass::C) || root > static_cast<int>(PitchClass::B) ||
        mode < static_cast<int>(Mode::Major) || mode > static_cast<int>(Mode::Locrian)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  out->items = nullptr;
  out->count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChordConfig config;
    config.min_duration = options->min_duration;
    config.smoothing_window = options->smoothing_window;
    config.threshold = options->threshold;
    config.use_triads_only = options->use_triads_only != 0;
    config.n_fft = options->n_fft;
    config.hop_length = options->hop_length;
    config.use_beat_sync = options->use_beat_sync != 0;
    config.use_hmm = options->use_hmm != 0;
    config.hmm_beam_width = options->hmm_beam_width;
    config.use_key_context = options->use_key_context != 0;
    config.key_root = from_c_pitch_class(options->key_root);
    config.key_mode = from_c_mode(options->key_mode);
    config.detect_inversions = options->detect_inversions != 0;
    config.chroma_method = options->chroma_method == 1 ? ChromaMethod::NNLS : ChromaMethod::STFT;

    ChordAnalyzer analyzer(audio, config);
    std::vector<std::string> labels =
        analyzer.functional_analysis(from_c_pitch_class(key_root), from_c_mode(key_mode));

    if (labels.empty()) {
      return SONARE_OK;
    }

    std::unique_ptr<char*[]> items(new char*[labels.size()]);
    size_t filled = 0;
    try {
      for (; filled < labels.size(); ++filled) {
        items[filled] = copy_string(labels[filled]);
      }
    } catch (...) {
      for (size_t i = 0; i < filled; ++i) sonare_free_string(items[i]);
      throw;
    }
    out->items = release_array(items);
    out->count = labels.size();
    return SONARE_OK;
  });
}

SonareError sonare_analyze_sections(const float* samples, size_t length, int sample_rate, int n_fft,
                                    int hop_length, float min_section_sec,
                                    SonareSectionResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_fft <= 0 || hop_length <= 0 || min_section_sec < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  out->sections = nullptr;
  out->section_count = 0;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    SectionConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.min_section_sec = min_section_sec;
    SectionAnalyzer analyzer(audio, config);
    const std::vector<Section>& sections = analyzer.sections();
    if (!sections.empty()) {
      auto data = std::make_unique<SonareSection[]>(sections.size());
      for (size_t i = 0; i < sections.size(); ++i) {
        data[i].type = static_cast<SonareSectionType>(sections[i].type);
        data[i].start = sections[i].start;
        data[i].end = sections[i].end;
        data[i].energy_level = sections[i].energy_level;
        data[i].confidence = sections[i].confidence;
      }
      out->sections = release_array(data);
      out->section_count = sections.size();
    }
    return SONARE_OK;
  });
}

SonareError sonare_analyze_melody(const float* samples, size_t length, int sample_rate, float fmin,
                                  float fmax, int frame_length, int hop_length, float threshold,
                                  SonareMelodyResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (fmin <= 0.0f || fmax <= fmin || frame_length <= 0 || hop_length <= 0 || threshold <= 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    MelodyConfig config;
    config.fmin = fmin;
    config.fmax = fmax;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.threshold = threshold;
    MelodyAnalyzer analyzer(audio, config);
    const MelodyContour& contour = analyzer.contour();
    out->pitch_range_octaves = contour.pitch_range_octaves;
    out->pitch_stability = contour.pitch_stability;
    out->mean_frequency = contour.mean_frequency;
    out->vibrato_rate = contour.vibrato_rate;
    if (!contour.pitches.empty()) {
      auto data = std::make_unique<SonareMelodyPoint[]>(contour.pitches.size());
      for (size_t i = 0; i < contour.pitches.size(); ++i) {
        data[i].time = contour.pitches[i].time;
        data[i].frequency = contour.pitches[i].frequency;
        data[i].confidence = contour.pitches[i].confidence;
      }
      out->points = release_array(data);
      out->point_count = contour.pitches.size();
    }
    return SONARE_OK;
  });
}
