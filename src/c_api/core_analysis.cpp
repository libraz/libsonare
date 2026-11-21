#include <array>

#include "c_api/core_internal.h"
#include "c_api/feature_input_domains.h"
#include "util/parameter_checks.h"

namespace {

// The C-layer fields: the struct size and the integer stand-ins for enums. The numeric
// fields are the core's rule, applied by check_chord_detection_options.
void validate_chord_detection_options(const SonareChordDetectionOptions& options) {
  sonare_c_detail::require_struct_size(options.struct_size, sizeof(SonareChordDetectionOptions),
                                       "options", "sonare_chord_detection_options_init");
  // Reject out-of-range enum-like fields instead of silently mapping them to a
  // default (chroma_method only documents 0 = STFT, 1 = NNLS, 2 = CQT). When key context
  // is enabled, also require an in-range key_root / key_mode.
  if (options.use_key_context != 0) {
    util::check::integer_in_range(options.key_root, static_cast<int>(PitchClass::C),
                                  static_cast<int>(PitchClass::B), "options.key_root");
    util::check::integer_in_range(options.key_mode, static_cast<int>(Mode::Major),
                                  static_cast<int>(Mode::Locrian), "options.key_mode");
  }
  util::check::integer_in_range(options.chroma_method, 0, 2, "options.chroma_method");
}

ChordConfig chord_config_from_options(const SonareChordDetectionOptions& options) {
  ChordConfig config;
  config.min_duration = options.min_duration_sec;
  config.smoothing_window = options.smoothing_window_sec;
  config.threshold = options.threshold;
  config.use_triads_only = options.use_triads_only != 0;
  config.n_fft = options.n_fft;
  config.hop_length = core_length(options.hop_length);
  config.use_beat_sync = options.use_beat_sync != 0;
  config.use_hmm = options.use_hmm != 0;
  config.hmm_beam_width = options.hmm_beam_width;
  config.use_key_context = options.use_key_context != 0;
  // validate_chord_detection_options admitted the key whenever key context reads it.
  if (config.use_key_context && (!from_c_pitch_class(options.key_root, &config.key_root) ||
                                 !from_c_mode(options.key_mode, &config.key_mode))) {
    throw SonareException(ErrorCode::InvalidParameter, "chord key context is out of range");
  }
  config.detect_inversions = options.detect_inversions != 0;
  switch (options.chroma_method) {
    case 1:
      config.chroma_method = ChromaMethod::NNLS;
      break;
    case 2:
      config.chroma_method = ChromaMethod::CQT;
      break;
    default:
      config.chroma_method = ChromaMethod::STFT;
      break;
  }
  // A non-zero tuning_auto leaves the tuning unset, which measures it.
  if (options.tuning_auto == 0) config.tuning = tuning_from_cents(options.tuning_cents);
  return config;
}

SonareError check_chord_detection_options(const SonareChordDetectionOptions& options) {
  return run_validation([&] {
    validate_chord_detection_options(options);
    validate_chord_config(chord_config_from_options(options));
  });
}

}  // namespace

SonareError sonare_analyze_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                               float bpm_max, float start_bpm, int n_fft, size_t hop_length,
                               int max_candidates, SonareBpmAnalysisResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = core_length(hop_length);
  if (const SonareError err = run_validation([&] {
        validate_bpm_config(config);
        validate_bpm_candidate_count(max_candidates);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "analyze_bpm", "samples", samples, length, sample_rate,
      [&](const Audio& audio) -> SonareError {
        BpmAnalyzer analyzer(audio, config);
        StagedOutput<SonareBpmAnalysisResult, &sonare_free_bpm_analysis_result> staged;
        staged->bpm = analyzer.bpm();
        staged->confidence = analyzer.confidence();

        std::vector<BpmCandidate> candidates = analyzer.candidates(max_candidates);
        staged->candidate_count = candidates.size();
        if (!candidates.empty()) {
          std::unique_ptr<SonareBpmCandidate[]> cands(new SonareBpmCandidate[candidates.size()]);
          for (size_t i = 0; i < candidates.size(); ++i) {
            cands[i].bpm = candidates[i].bpm;
            cands[i].confidence = candidates[i].confidence;
          }
          staged->candidates = release_array(cands);
        }

        const std::vector<float>& autocorr = analyzer.autocorrelation();
        staged->autocorrelation_count = autocorr.size();
        staged->autocorrelation = copy_vector(autocorr);

        const std::vector<float>& tempogram = analyzer.tempogram();
        staged->tempogram_count = tempogram.size();
        staged->tempogram = copy_vector(tempogram);
        staged.publish(out);
        return SONARE_OK;
      });
}

void sonare_analyze_impulse_response_options_init(SonareAnalyzeImpulseResponseOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->min_decay_db = 30.0f;
}

SonareError sonare_analyze_impulse_response(const float* samples, size_t length, int sample_rate,
                                            int n_octave_bands,
                                            const SonareAnalyzeImpulseResponseOptions* options,
                                            SonareAcousticResult* out) {
  SONARE_C_API_ENTRY;

  // Zero the whole struct up front, BEFORE any validating early-return (this
  // function's own n_octave_bands check or a validate_audio_params failure
  // inside run_offline), so a rejected input always leaves a fully-initialized
  // result: the scalar fields (rt60/edt/c50/c80/d50/confidence/is_blind) as
  // well as the band pointers / band_count are all defined, not caller
  // garbage. Otherwise sonare_free_acoustic_result(&r) would delete[] an
  // uninitialised pointer.
  SONARE_C_ZERO_OUTS(out);
  SonareAnalyzeImpulseResponseOptions resolved;
  sonare_analyze_impulse_response_options_init(&resolved);
  if (options != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(options->struct_size, sizeof(SonareAnalyzeImpulseResponseOptions),
                              "options", "sonare_analyze_impulse_response_options_init");
        });
        err != SONARE_OK) {
      return err;
    }
    resolved = *options;
  }
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  config.min_decay_db = resolved.min_decay_db;
  if (const SonareError err = run_validation([&] { validate_acoustic_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("analyze_impulse_response", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       fill_acoustic_result(sonare::analyze_impulse_response(audio, config), out);
                       return SONARE_OK;
                     });
}

SonareError sonare_detect_acoustic(const float* samples, size_t length, int sample_rate,
                                   int n_octave_bands, int n_third_octave_subbands,
                                   float min_decay_db, float noise_floor_margin_db,
                                   SonareAcousticResult* out) {
  SONARE_C_API_ENTRY;

  // Zero the whole struct up front, BEFORE any validating early-return (this
  // function's own scalar checks or a validate_audio_params failure inside
  // run_offline), so a rejected input always leaves a fully-initialized
  // result: the scalar fields (rt60/edt/c50/c80/d50/confidence/is_blind) as
  // well as the band pointers / band_count are all defined, not caller
  // garbage. Otherwise sonare_free_acoustic_result(&r) would delete[] an
  // uninitialised pointer.
  SONARE_C_ZERO_OUTS(out);
  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = n_octave_bands;
  config.n_third_octave_subbands = n_third_octave_subbands;
  config.min_decay_db = min_decay_db;
  config.noise_floor_margin_db = noise_floor_margin_db;
  if (const SonareError err = run_validation([&] { validate_acoustic_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("detect_acoustic", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       fill_acoustic_result(sonare::detect_acoustic(audio, config), out);
                       return SONARE_OK;
                     });
}

SonareError sonare_analyze_rhythm(const float* samples, size_t length, int sample_rate,
                                  float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  size_t hop_length, SonareRhythmResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  // Built inside each guarded call: RhythmConfig allocates, and a throw here must not escape.
  const auto make_config = [&] {
    RhythmConfig config;
    config.bpm_min = bpm_min;
    config.bpm_max = bpm_max;
    config.start_bpm = start_bpm;
    config.n_fft = n_fft;
    config.hop_length = core_length(hop_length);
    return config;
  };
  if (const SonareError err = run_validation([&] { validate_rhythm_config(make_config()); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("analyze_rhythm", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       RhythmAnalyzer analyzer(audio, make_config());
                       RhythmFeatures features = analyzer.features();
                       StagedOutput<SonareRhythmResult, &sonare_free_rhythm_result> staged;
                       staged->bpm = analyzer.bpm();
                       staged->time_signature.numerator = features.time_signature.numerator;
                       staged->time_signature.denominator = features.time_signature.denominator;
                       staged->time_signature.confidence = features.time_signature.confidence;
                       staged->groove_type = to_c_groove_type(features.groove_type);
                       staged->syncopation = features.syncopation;
                       staged->pattern_regularity = features.pattern_regularity;
                       staged->tempo_stability = features.tempo_stability;

                       const std::vector<float>& intervals = analyzer.beat_intervals();
                       staged->beat_interval_count = intervals.size();
                       staged->beat_intervals = copy_vector(intervals);
                       staged.publish(out);
                       return SONARE_OK;
                     });
}

SonareError sonare_analyze_dynamics(const float* samples, size_t length, int sample_rate,
                                    float window_sec, size_t hop_length,
                                    float compression_threshold_db, SonareDynamicsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  DynamicsConfig config;
  config.window_sec = window_sec;
  config.hop_length = core_length(hop_length);
  config.compression_threshold = compression_threshold_db;
  if (const SonareError err = run_validation([&] { validate_dynamics_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("analyze_dynamics", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       DynamicsAnalyzer analyzer(audio, config);
                       const Dynamics& dynamics = analyzer.dynamics();
                       out->dynamic_range_db = dynamics.dynamic_range_db;
                       out->peak_db = dynamics.peak_db;
                       out->rms_db = dynamics.rms_db;
                       out->crest_factor = dynamics.crest_factor;
                       out->loudness_range_lu = dynamics.loudness_range_lu;
                       out->is_compressed = dynamics.is_compressed ? 1 : 0;

                       const LoudnessCurve& curve = analyzer.loudness_curve();
                       size_t count = std::min(curve.times.size(), curve.rms_db.size());
                       out->loudness_count = count;
                       if (count > 0) {
                         std::unique_ptr<float[]> times(new float[count]);
                         std::unique_ptr<float[]> rms_db(new float[count]);
                         std::memcpy(times.get(), curve.times.data(), count * sizeof(float));
                         std::memcpy(rms_db.get(), curve.rms_db.data(), count * sizeof(float));
                         out->loudness_times_sec = release_array(times);
                         out->loudness_rms_db = release_array(rms_db);
                       }
                       return SONARE_OK;
                     });
}

SonareError sonare_analyze_timbre(const float* samples, size_t length, int sample_rate, int n_fft,
                                  size_t hop_length, int n_mels, int n_mfcc, float window_sec,
                                  SonareTimbreResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  TimbreConfig config;
  config.n_fft = n_fft;
  config.hop_length = core_length(hop_length);
  config.n_mels = n_mels;
  config.n_mfcc = n_mfcc;
  config.window_sec = window_sec;
  if (const SonareError err = run_validation([&] { validate_timbre_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "analyze_timbre", "samples", samples, length, sample_rate,
      [&](const Audio& audio) -> SonareError {
        TimbreAnalyzer analyzer(audio, config);
        const Timbre& timbre = analyzer.timbre();
        StagedOutput<SonareTimbreResult, &sonare_free_timbre_result> staged;
        staged->brightness = timbre.brightness;
        staged->warmth = timbre.warmth;
        staged->density = timbre.density;
        staged->roughness = timbre.roughness;
        staged->complexity = timbre.complexity;

        const std::vector<float>& centroid = analyzer.spectral_centroid();
        staged->spectral_centroid_count = centroid.size();
        staged->spectral_centroid = copy_vector(centroid);

        const std::vector<float>& flatness = analyzer.spectral_flatness();
        staged->spectral_flatness_count = flatness.size();
        staged->spectral_flatness = copy_vector(flatness);

        const std::vector<float>& rolloff = analyzer.spectral_rolloff();
        staged->spectral_rolloff_count = rolloff.size();
        staged->spectral_rolloff = copy_vector(rolloff);

        const std::vector<Timbre>& over_time = analyzer.timbre_over_time();
        staged->timbre_over_time_count = over_time.size();
        if (!over_time.empty()) {
          std::unique_ptr<SonareTimbreFrame[]> frames(new SonareTimbreFrame[over_time.size()]);
          for (size_t i = 0; i < over_time.size(); ++i) {
            frames[i].brightness = over_time[i].brightness;
            frames[i].warmth = over_time[i].warmth;
            frames[i].density = over_time[i].density;
            frames[i].roughness = over_time[i].roughness;
            frames[i].complexity = over_time[i].complexity;
          }
          staged->timbre_over_time = release_array(frames);
        }
        staged.publish(out);
        return SONARE_OK;
      });
}

void sonare_chord_detection_options_init(SonareChordDetectionOptions* options) {
  if (options == nullptr) return;
  // Copied from the core defaults rather than restated, so the two cannot part.
  const ChordConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->min_duration_sec = config.min_duration;
  options->smoothing_window_sec = config.smoothing_window;
  options->threshold = config.threshold;
  options->use_triads_only = config.use_triads_only ? 1 : 0;
  options->n_fft = config.n_fft;
  options->hop_length = static_cast<size_t>(config.hop_length);
  options->use_beat_sync = config.use_beat_sync ? 1 : 0;
  options->use_hmm = config.use_hmm ? 1 : 0;
  options->hmm_beam_width = config.hmm_beam_width;
  options->use_key_context = config.use_key_context ? 1 : 0;
  options->key_root = static_cast<SonarePitchClass>(static_cast<int>(config.key_root));
  options->key_mode = static_cast<SonareMode>(static_cast<int>(config.key_mode));
  options->detect_inversions = config.detect_inversions ? 1 : 0;
  options->chroma_method = static_cast<SonareChromaMethod>(static_cast<int>(config.chroma_method));
  options->tuning_cents = config.tuning ? tuning_to_cents(*config.tuning) : 0.0f;
  options->tuning_auto = config.tuning ? 0 : 1;
}

SonareError sonare_detect_chords(const float* samples, size_t length, int sample_rate,
                                 const SonareChordDetectionOptions* options,
                                 SonareChordAnalysisResult* out) {
  SONARE_C_API_ENTRY;

  // Zero the owned out-pointer BEFORE any validating early-return so a
  // rejected input always leaves a NULL owned pointer; otherwise
  // sonare_free_chord_analysis_result(&r) would delete[] an uninitialised
  // pointer.
  if (out != nullptr) *out = {};
  SonareChordDetectionOptions resolved;
  sonare_chord_detection_options_init(&resolved);
  if (options != nullptr) {
    if (const SonareError err = check_chord_detection_options(*options); err != SONARE_OK) {
      return err;
    }
    resolved = *options;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("detect_chords", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       const ChordConfig config = chord_config_from_options(resolved);

                       ChordAnalyzer analyzer(audio, config);
                       fill_chord_result(analyzer.chords(), analyzer.tuning(), out);
                       return SONARE_OK;
                     });
}

namespace {

// Copies labels into a heap-owned SonareStringArray; frees what it copied on failure.
void fill_string_array(const std::vector<std::string>& labels, SonareStringArray* out) {
  if (labels.empty()) return;
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
}

// Roman numeral and harmonic function of each chord in a key, into two string arrays.
void label_chords(const std::vector<Chord>& chords, PitchClass tonic, Mode mode,
                  SonareStringArray* roman, SonareStringArray* functions) {
  std::vector<std::string> numerals;
  std::vector<std::string> names;
  numerals.reserve(chords.size());
  names.reserve(chords.size());
  for (const Chord& chord : chords) {
    numerals.push_back(ChordAnalyzer::chord_to_roman_numeral(chord, tonic, mode));
    names.push_back(ChordAnalyzer::chord_function(chord, tonic, mode));
  }
  fill_string_array(numerals, roman);
  try {
    fill_string_array(names, functions);
  } catch (...) {
    sonare_free_string_array(roman);
    throw;
  }
}

}  // namespace

SonareError sonare_chord_functional_analysis(const float* samples, size_t length, int sample_rate,
                                             const SonareChordDetectionOptions* options,
                                             SonarePitchClass key_root, SonareMode key_mode,
                                             SonareChordAnalysisResult* chords,
                                             SonareStringArray* roman,
                                             SonareStringArray* functions) {
  SONARE_C_API_ENTRY;

  // Zero the owned out-pointers BEFORE any validating early-return so a
  // rejected input always leaves NULL owned pointers; otherwise the free
  // functions would delete[] uninitialised pointers.
  if (chords != nullptr) *chords = {};
  if (roman != nullptr) {
    roman->items = nullptr;
    roman->count = 0;
  }
  if (functions != nullptr) {
    functions->items = nullptr;
    functions->count = 0;
  }
  SONARE_C_REQUIRE_NONNULL(options);
  if (const SonareError err = check_chord_detection_options(*options); err != SONARE_OK) {
    return err;
  }
  // The analysis key drives the Roman-numeral labelling.
  if (const SonareError err = run_validation([&] {
        util::check::integer_in_range(key_root, static_cast<int>(PitchClass::C),
                                      static_cast<int>(PitchClass::B), "key_root");
        util::check::integer_in_range(key_mode, static_cast<int>(Mode::Major),
                                      static_cast<int>(Mode::Locrian), "key_mode");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(chords);
  SONARE_C_REQUIRE_NONNULL(roman);
  SONARE_C_REQUIRE_NONNULL(functions);

  return run_offline("chord_functional_analysis", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       PitchClass label_root = PitchClass::C;
                       Mode label_mode = Mode::Major;
                       (void)from_c_pitch_class(key_root, &label_root);
                       (void)from_c_mode(key_mode, &label_mode);
                       const ChordConfig config = chord_config_from_options(*options);

                       ChordAnalyzer analyzer(audio, config);
                       fill_chord_result(analyzer.chords(), analyzer.tuning(), chords);
                       try {
                         label_chords(analyzer.chords(), label_root, label_mode, roman, functions);
                       } catch (...) {
                         sonare_free_chord_analysis_result(chords);
                         throw;
                       }
                       return SONARE_OK;
                     });
}

SonareError sonare_chord_functions(const SonareChord* chords, size_t count,
                                   SonarePitchClass key_root, SonareMode key_mode,
                                   SonareStringArray* roman, SonareStringArray* functions) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  if (roman != nullptr) {
    roman->items = nullptr;
    roman->count = 0;
  }
  if (functions != nullptr) {
    functions->items = nullptr;
    functions->count = 0;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(chords, count);
  util::check::integer_within(count, sonare_c_detail::kChordLabelCountDomain, "count");
  PitchClass label_root = PitchClass::C;
  Mode label_mode = Mode::Major;
  if (!from_c_pitch_class(key_root, &label_root)) {
    util::check::integer_in_range(key_root, static_cast<int>(PitchClass::C),
                                  static_cast<int>(PitchClass::B), "key_root");
  }
  if (!from_c_mode(key_mode, &label_mode)) {
    util::check::integer_in_range(key_mode, static_cast<int>(Mode::Major),
                                  static_cast<int>(Mode::Locrian), "key_mode");
  }
  std::vector<Chord> input;
  input.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Chord chord{};
    // The element subject is built only on refusal, so a valid call allocates nothing per chord.
    if (!from_c_pitch_class(chords[i].root, &chord.root)) {
      util::check::integer_in_range(chords[i].root, static_cast<int>(PitchClass::C),
                                    static_cast<int>(PitchClass::B),
                                    ("chords[" + std::to_string(i) + "].root").c_str());
    }
    if (!ordinal_to_enum(chords[i].quality, 0, SONARE_CHORD_QUALITY_COUNT - 1, &chord.quality)) {
      util::check::integer_in_range(chords[i].quality, 0, SONARE_CHORD_QUALITY_COUNT - 1,
                                    ("chords[" + std::to_string(i) + "].quality").c_str());
    }
    chord.bass = chord.root;
    input.push_back(chord);
  }
  SONARE_C_REQUIRE_NONNULL(roman);
  SONARE_C_REQUIRE_NONNULL(functions);
  label_chords(input, label_root, label_mode, roman, functions);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_section_options_init(SonareSectionOptions* options) {
  if (options == nullptr) return;
  // Copied from the core defaults rather than restated, so the two cannot part.
  const SectionConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = config.n_fft;
  options->hop_length = static_cast<size_t>(config.hop_length);
  options->min_section_sec = config.min_section_sec;
  options->threshold = config.boundary_threshold;
}

namespace {

// The core's config member names, as the options struct spells them.
const std::array<CName, 4> kSectionOptionNames{{{"n_fft", "options.n_fft"},
                                                {"hop_length", "options.hop_length"},
                                                {"min_section_sec", "options.min_section_sec"},
                                                {"boundary_threshold", "options.threshold"}}};

const std::array<CName, 11> kBoundaryOptionNames{
    {{"n_fft", "options.n_fft"},
     {"hop_length", "options.hop_length"},
     {"kernel_size", "options.kernel_size"},
     {"n_mfcc", "options.n_mfcc"},
     {"n_chroma", "options.n_chroma"},
     {"threshold", "options.threshold"},
     {"absolute_threshold", "options.absolute_threshold"},
     {"peak_distance", "options.peak_distance_sec"},
     {"reference_window", "options.reference_window_sec"},
     {"use_mfcc", "options.use_mfcc"},
     {"use_chroma", "options.use_chroma"}}};

}  // namespace

SonareError sonare_analyze_sections(const float* samples, size_t length, int sample_rate,
                                    const SonareSectionOptions* options, SonareSectionResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(options);
  SectionConfig config;
  config.n_fft = options->n_fft;
  config.hop_length = core_length(options->hop_length);
  config.min_section_sec = options->min_section_sec;
  config.boundary_threshold = options->threshold;
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareSectionOptions),
                                             "options", "sonare_section_options_init");
        with_c_names(kSectionOptionNames, [&] { validate_section_config(config); });
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("analyze_sections", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       SectionAnalyzer analyzer(audio, config);
                       const std::vector<Section>& sections = analyzer.sections();
                       std::unique_ptr<char[]> form(copy_string(analyzer.form()));
                       if (!sections.empty()) {
                         auto data = std::make_unique<SonareSection[]>(sections.size());
                         for (size_t i = 0; i < sections.size(); ++i) {
                           data[i].type = static_cast<SonareSectionType>(sections[i].type);
                           data[i].start_sec = sections[i].start;
                           data[i].end_sec = sections[i].end;
                           data[i].energy_level = sections[i].energy_level;
                           data[i].confidence = sections[i].confidence;
                           data[i].group = static_cast<int32_t>(sections[i].group);
                         }
                         out->sections = release_array(data);
                         out->section_count = sections.size();
                       }
                       out->form = form.release();
                       return SONARE_OK;
                     });
}

void sonare_boundary_options_init(SonareBoundaryOptions* options) {
  if (options == nullptr) return;
  // Copied from the core defaults rather than restated, so the two cannot part.
  const BoundaryConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = config.n_fft;
  options->hop_length = static_cast<size_t>(config.hop_length);
  options->kernel_size = config.kernel_size;
  options->threshold = config.threshold;
  options->absolute_threshold = config.absolute_threshold;
  options->n_mfcc = config.n_mfcc;
  options->n_chroma = config.n_chroma;
  options->peak_distance_sec = config.peak_distance;
  options->use_mfcc = config.use_mfcc ? 1 : 0;
  options->use_chroma = config.use_chroma ? 1 : 0;
  options->reference_window_sec = config.reference_window;
}

SonareError sonare_detect_boundaries(const float* samples, size_t length, int sample_rate,
                                     const SonareBoundaryOptions* options,
                                     SonareBoundaryResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(options);
  BoundaryConfig config;
  config.n_fft = options->n_fft;
  config.hop_length = core_length(options->hop_length);
  config.kernel_size = options->kernel_size;
  config.threshold = options->threshold;
  config.absolute_threshold = options->absolute_threshold;
  config.n_mfcc = options->n_mfcc;
  config.n_chroma = options->n_chroma;
  config.peak_distance = options->peak_distance_sec;
  config.use_mfcc = options->use_mfcc != 0;
  config.use_chroma = options->use_chroma != 0;
  config.reference_window = options->reference_window_sec;
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareBoundaryOptions),
                                             "options", "sonare_boundary_options_init");
        with_c_names(kBoundaryOptionNames, [&] { validate_boundary_config(config); });
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("detect_boundaries", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       BoundaryDetector detector(audio, config);
                       const std::vector<Boundary>& boundaries = detector.boundaries();
                       if (!boundaries.empty()) {
                         auto data = std::make_unique<SonareBoundary[]>(boundaries.size());
                         for (size_t i = 0; i < boundaries.size(); ++i) {
                           data[i].time_sec = boundaries[i].time;
                           data[i].frame = boundaries[i].frame;
                           data[i].strength = boundaries[i].strength;
                         }
                         out->boundaries = release_array(data);
                         out->boundary_count = boundaries.size();
                       }
                       const std::vector<float>& novelty = detector.novelty_curve();
                       if (!novelty.empty()) {
                         auto curve = std::make_unique<float[]>(novelty.size());
                         std::copy(novelty.begin(), novelty.end(), curve.get());
                         out->novelty_curve = release_array(curve);
                         out->novelty_length = novelty.size();
                       }
                       out->novelty_peak = detector.novelty_peak();
                       // The analysis rate, not the caller's: input above 22.05 kHz is resampled
                       // before any feature is computed, and `frame` indexes that grid.
                       out->sample_rate = detector.sample_rate();
                       out->hop_length = static_cast<size_t>(detector.hop_length());
                       out->n_frames = detector.n_frames();
                       out->frame_stride = detector.frame_stride();
                       return SONARE_OK;
                     });
}

void sonare_analyze_melody_options_init(SonareAnalyzeMelodyOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->use_pyin = 0;
  options->center = 1;
}

SonareError sonare_analyze_melody(const float* samples, size_t length, int sample_rate,
                                  float f0_min_hz, float f0_max_hz, size_t frame_length,
                                  size_t hop_length, float threshold,
                                  const SonareAnalyzeMelodyOptions* options,
                                  SonareMelodyResult* out) {
  SONARE_C_API_ENTRY;

  // Zero the whole struct BEFORE any validating early-return so a rejected
  // input always leaves a NULL owned pointer; otherwise
  // sonare_free_melody_result(&r) would delete[] an uninitialised pointer.
  SONARE_C_ZERO_OUTS(out);
  SonareAnalyzeMelodyOptions resolved;
  sonare_analyze_melody_options_init(&resolved);
  if (options != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(options->struct_size, sizeof(SonareAnalyzeMelodyOptions), "options",
                              "sonare_analyze_melody_options_init");
        });
        err != SONARE_OK) {
      return err;
    }
    resolved = *options;
  }
  MelodyConfig config;
  config.fmin = f0_min_hz;
  config.fmax = f0_max_hz;
  config.frame_length = core_length(frame_length);
  config.hop_length = core_length(hop_length);
  config.threshold = threshold;
  config.use_pyin = resolved.use_pyin != 0;
  config.center = resolved.center != 0;
  if (const SonareError err = run_validation([&] { validate_melody_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("analyze_melody", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       MelodyAnalyzer analyzer(audio, config);
                       const MelodyContour& contour = analyzer.contour();
                       out->pitch_range_octaves = contour.pitch_range_octaves;
                       out->pitch_stability = contour.pitch_stability;
                       out->mean_frequency_hz = contour.mean_frequency;
                       out->vibrato_rate_hz = contour.vibrato_rate;
                       if (!contour.pitches.empty()) {
                         auto data = std::make_unique<SonareMelodyPoint[]>(contour.pitches.size());
                         for (size_t i = 0; i < contour.pitches.size(); ++i) {
                           data[i].time_sec = contour.pitches[i].time;
                           data[i].frequency_hz = contour.pitches[i].frequency;
                           data[i].confidence = contour.pitches[i].confidence;
                         }
                         out->points = release_array(data);
                         out->point_count = contour.pitches.size();
                       }
                       return SONARE_OK;
                     });
}
