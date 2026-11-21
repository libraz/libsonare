#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>

#include "analysis/analysis_json.h"
#include "analysis/meter_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "c_api/core_internal.h"
#include "util/parameter_checks.h"

namespace {

// Builds the config from arguments validate_key_arguments already admitted.
void make_key_config(int n_fft, int hop_length, int use_hpss, int loudness_weighted,
                     float chroma_high_pass_hz, const SonareMode* modes, size_t mode_count,
                     SonareKeyProfileType profile_type, const char* genre_hint,
                     KeyConfig* out_config) {
  out_config->n_fft = n_fft;
  out_config->hop_length = hop_length;
  out_config->use_hpss = use_hpss != 0;
  out_config->loudness_weighted = loudness_weighted != 0;
  out_config->high_pass_hz = chroma_high_pass_hz;
  (void)fill_key_modes(modes, mode_count, out_config);
  (void)fill_key_profile(profile_type, out_config);
  if (genre_hint != nullptr && genre_hint[0] != '\0') {
    out_config->genre_hint = genre_hint;
  }
}

// A C tuning pair as the core's optional: a non-zero tuning_auto leaves it unset, which measures.
std::optional<float> tuning_from_c(float tuning_cents, int tuning_auto) {
  return tuning_auto != 0 ? std::nullopt : std::optional<float>(tuning_from_cents(tuning_cents));
}

// The key arguments in header order: the scalars through the core's rule for them, then the mode
// list and the profile selector, which are this layer's own.
SonareError validate_key_arguments(int n_fft, int hop_length, int use_hpss,
                                   float chroma_high_pass_hz, const SonareMode* modes,
                                   size_t mode_count, SonareKeyProfileType profile_type) {
  if (const SonareError err = run_validation([&] {
        KeyConfig config;
        config.n_fft = n_fft;
        config.hop_length = hop_length;
        config.use_hpss = use_hpss != 0;
        config.high_pass_hz = chroma_high_pass_hz;
        validate_key_config(config);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(modes, mode_count);
  return run_validation([&] {
    for (size_t i = 0; i < mode_count; ++i) {
      Mode mode = Mode::Major;
      // The element subject is built only on refusal.
      if (!from_c_mode(modes[i], &mode)) {
        util::check::integer_in_range(modes[i], static_cast<int>(Mode::Major),
                                      static_cast<int>(Mode::Locrian),
                                      ("modes[" + std::to_string(i) + "]").c_str());
      }
    }
    util::check::integer_in_range(profile_type, SONARE_KEY_PROFILE_AUTO,
                                  SONARE_KEY_PROFILE_BELLMAN_BUDGE, "profile_type");
  });
}

}  // namespace

// Quick detection functions

SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                              float bpm_max, float* out_bpm) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_bpm);
  if (const SonareError err =
          validate_audio_params("detect_bpm", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  if (const SonareError err = run_validation([&] { validate_bpm_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_bpm);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        *out_bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate(), config);
        return SONARE_OK;
      });
}

void sonare_detect_key_options_init(SonareDetectKeyOptions* options) {
  if (options == nullptr) return;
  const KeyConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = config.n_fft;
  options->hop_length = config.hop_length;
  options->use_hpss = config.use_hpss ? 1 : 0;
  options->loudness_weighted = config.loudness_weighted ? 1 : 0;
  options->chroma_high_pass_hz = config.high_pass_hz;
  options->profile_type = SONARE_KEY_PROFILE_AUTO;
  options->tuning_auto = 1;
}

namespace {

// The options a key entry reads: the caller's after the layout check, else the defaults.
SonareError resolve_key_options(const SonareDetectKeyOptions* options,
                                SonareDetectKeyOptions* defaults,
                                const SonareDetectKeyOptions** out) {
  if (options == nullptr) {
    sonare_detect_key_options_init(defaults);
    *out = defaults;
    return SONARE_OK;
  }
  *out = options;
  return run_validation([&] {
    sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareDetectKeyOptions),
                                         "options", "sonare_detect_key_options_init");
  });
}

}  // namespace

SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              const SonareDetectKeyOptions* options, SonareKey* out_key,
                              float* out_tuning_cents) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_key, out_tuning_cents);
  if (const SonareError err =
          validate_audio_params("detect_key", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareDetectKeyOptions defaults;
  const SonareDetectKeyOptions* opt = nullptr;
  if (const SonareError err = resolve_key_options(options, &defaults, &opt); err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = validate_key_arguments(opt->n_fft, opt->hop_length, opt->use_hpss,
                                                     opt->chroma_high_pass_hz, opt->modes,
                                                     opt->mode_count, opt->profile_type);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_key);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        KeyConfig config;
        make_key_config(opt->n_fft, opt->hop_length, opt->use_hpss, opt->loudness_weighted,
                        opt->chroma_high_pass_hz, opt->modes, opt->mode_count, opt->profile_type,
                        opt->genre_hint, &config);
        config.tuning = tuning_from_c(opt->tuning_cents, opt->tuning_auto);
        float used = 0.0f;
        Key key = quick::detect_key(audio.data(), audio.size(), audio.sample_rate(), config, &used);
        out_key->root = static_cast<SonarePitchClass>(key.root);
        out_key->mode = static_cast<SonareMode>(key.mode);
        out_key->confidence = key.confidence;
        if (out_tuning_cents != nullptr) *out_tuning_cents = tuning_to_cents(used);
        return SONARE_OK;
      });
}

SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         const SonareDetectKeyOptions* options,
                                         SonareKeyCandidate** out_candidates, size_t* out_count,
                                         float* out_tuning_cents) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_candidates, out_count, out_tuning_cents);
  if (const SonareError err =
          validate_audio_params("detect_key_candidates", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareDetectKeyOptions defaults;
  const SonareDetectKeyOptions* opt = nullptr;
  if (const SonareError err = resolve_key_options(options, &defaults, &opt); err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = validate_key_arguments(opt->n_fft, opt->hop_length, opt->use_hpss,
                                                     opt->chroma_high_pass_hz, opt->modes,
                                                     opt->mode_count, opt->profile_type);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_candidates);
  SONARE_C_REQUIRE_NONNULL(out_count);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        KeyConfig config;
        make_key_config(opt->n_fft, opt->hop_length, opt->use_hpss, opt->loudness_weighted,
                        opt->chroma_high_pass_hz, opt->modes, opt->mode_count, opt->profile_type,
                        opt->genre_hint, &config);
        // Resolved here so the value used can be reported; measure_tuning reads the analysis rate.
        config.tuning =
            opt->tuning_auto != 0 ? measure_tuning(audio) : tuning_from_cents(opt->tuning_cents);

        const auto candidates =
            quick::detect_key_candidates(audio.data(), audio.size(), audio.sample_rate(), config);
        if (out_tuning_cents != nullptr) *out_tuning_cents = tuning_to_cents(*config.tuning);
        if (candidates.empty()) {
          return SONARE_OK;
        }

        // The count is published with the array, never ahead of an allocation that can throw.
        std::unique_ptr<SonareKeyCandidate[]> out(new SonareKeyCandidate[candidates.size()]);
        for (size_t i = 0; i < candidates.size(); ++i) {
          out[i].key.root = static_cast<SonarePitchClass>(candidates[i].key.root);
          out[i].key.mode = static_cast<SonareMode>(candidates[i].key.mode);
          out[i].key.confidence = candidates[i].key.confidence;
          out[i].correlation = candidates[i].correlation;
        }
        *out_candidates = out.release();
        *out_count = candidates.size();
        return SONARE_OK;
      });
}

SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate, float bpm_min,
                                float bpm_max, float** out_times, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  if (const SonareError err =
          validate_audio_params("detect_beats", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  BpmConfig tempo;
  tempo.bpm_min = bpm_min;
  tempo.bpm_max = bpm_max;
  if (const SonareError err = run_validation([&] { validate_bpm_config(tempo); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);
  BeatConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        std::vector<float> beats =
            quick::detect_beats(audio.data(), audio.size(), audio.sample_rate(), config);
        return copy_vector(beats, out_times, out_count);
      });
}

SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  if (const SonareError err =
          validate_audio_params("detect_downbeats", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        std::vector<float> downbeats =
            quick::detect_downbeats(audio.data(), audio.size(), audio.sample_rate());
        return copy_vector(downbeats, out_times, out_count);
      });
}

void sonare_onset_detect_config_init(SonareOnsetDetectConfig* config) {
  if (config == nullptr) return;
  const sonare::OnsetDetectConfig native;
  *config = {};
  config->struct_size = sizeof(*config);
  config->n_fft = native.n_fft;
  config->hop_length = native.hop_length;
  config->threshold = std::numeric_limits<float>::quiet_NaN();
  config->pre_max = native.pre_max;
  config->post_max = native.post_max;
  config->pre_avg = native.pre_avg;
  config->post_avg = native.post_avg;
  config->delta = native.delta;
  config->wait = native.wait;
  config->backtrack = native.backtrack ? 1 : 0;
  config->backtrack_range = native.backtrack_range;
}

SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 const SonareOnsetDetectConfig* config, float** out_times,
                                 size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_times, out_count);
  if (const SonareError err =
          validate_audio_params("detect_onsets", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (config == nullptr) {
    SONARE_C_REQUIRE_NONNULL(out_times);
    SONARE_C_REQUIRE_NONNULL(out_count);
    return run_prevalidated_offline(
        samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
          std::vector<float> onsets =
              quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate());
          return copy_vector(onsets, out_times, out_count);
        });
  }
  // backtrack is a C byte standing in for a bool, so its range is this layer's own. A NaN
  // threshold is the adaptive path; a given one is a real minimum strength, above 0.
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(config->struct_size, sizeof(SonareOnsetDetectConfig),
                                             "config", "sonare_onset_detect_config_init");
        util::check::integer_in_range(config->backtrack, 0, 1, "config.backtrack");
        if (!std::isnan(config->threshold)) {
          util::check::greater_than(config->threshold, 0.0, "config.threshold");
        }
      });
      err != SONARE_OK) {
    return err;
  }

  sonare::OnsetDetectConfig native;
  native.n_fft = config->n_fft;
  native.hop_length = config->hop_length;
  native.threshold = std::isnan(config->threshold) ? 0.0f : config->threshold;
  native.pre_max = config->pre_max;
  native.post_max = config->post_max;
  native.pre_avg = config->pre_avg;
  native.post_avg = config->post_avg;
  native.delta = config->delta;
  native.wait = config->wait;
  native.backtrack = config->backtrack != 0;
  native.backtrack_range = config->backtrack_range;
  if (const SonareError err = run_validation([&] { validate_onset_detect_config(native); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_times);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        // Through quick:: rather than the core: going direct dropped the resample, which
        // reinterprets every frame-counted field in the config.
        std::vector<float> onsets =
            quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate(), native);
        return copy_vector(onsets, out_times, out_count);
      });
}

// Full quick analysis. Although SonareAnalysisResult is a flat C struct, it
// still exposes meter/beat data, so this intentionally pays the full quick
// analysis cost. Use sonare_detect_bpm/key/beats for cheaper single-purpose
// queries.

SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out) {
  SONARE_C_API_ENTRY;

  // Zero the whole struct up front so a rejected input never leaves an inconsistent
  // (null beat_times, garbage beat_count) pair (matches sonare_analyze_melody).
  SONARE_C_ZERO_OUTS(out);
  if (const SonareError err =
          validate_audio_params("analyze", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        AnalysisResult result = quick::analyze(audio.data(), audio.size(), audio.sample_rate());

        return fill_analysis_result(result, out);
      });
}

// Full analysis serialized to a camelCase JSON object (chords, sections,
// timbre, dynamics, rhythm, melody, form) — the rich counterpart to
// sonare_analyze, which only fills the flat bpm/key/beats struct. The schema is
// the single source of truth in analysis_result_to_json and is mirrored by the
// WASM native object. *out_json is heap-allocated; free with sonare_free_string.
static_assert(SONARE_MAX_METER_CANDIDATE_NUMERATORS == sonare::kMaxMeterCandidateNumerators,
              "The C array capacity and the core's validation limit must agree, or a list the "
              "core accepts would not fit the flat struct");

void sonare_music_analyze_options_init(SonareMusicAnalyzeOptions* options) {
  if (options == nullptr) return;
  const MusicAnalyzerConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->n_fft = config.n_fft;
  options->hop_length = config.hop_length;
  options->bpm_min = config.bpm_min;
  options->bpm_max = config.bpm_max;
  options->start_bpm = config.start_bpm;
  options->use_triads_only = config.use_triads_only ? 1 : 0;
  options->use_hpss = config.use_hpss ? 1 : 0;
  options->chroma_high_pass_hz = config.chroma_highpass_hz;
  options->use_bass_weighted = config.use_bass_weighted ? 1 : 0;
  options->chroma_hop_multiplier = config.chroma_hop_multiplier;
  options->use_chord_hmm = config.use_chord_hmm ? 1 : 0;
  options->use_chord_key_context = config.use_chord_key_context ? 1 : 0;
  options->chord_hmm_beam_width = config.chord_hmm_beam_width;
  options->detect_chord_inversions = config.detect_chord_inversions ? 1 : 0;
  options->adaptive_tempo = config.adaptive_tempo ? 1 : 0;
  options->tempo_update_interval_beats = config.tempo_update_interval_beats;
  options->compute_tempo_curve = config.compute_tempo_curve ? 1 : 0;
  options->meter_denominator = config.meter_denominator;
  options->tuning_cents = config.tuning ? tuning_to_cents(*config.tuning) : 0.0f;
  options->tuning_auto = config.tuning ? 0 : 1;
  const size_t count = std::min(config.meter_candidate_numerators.size(),
                                static_cast<size_t>(SONARE_MAX_METER_CANDIDATE_NUMERATORS));
  for (size_t i = 0; i < count; ++i) {
    options->meter_candidate_numerators[i] = config.meter_candidate_numerators[i];
  }
  options->meter_candidate_numerator_count = static_cast<int>(count);
}

namespace {

// The one mapping from the flat options struct to the core config. Field rules
// live in validate_config, which MusicAnalyzer's constructor applies.
MusicAnalyzerConfig music_analyzer_config_from_options(const SonareMusicAnalyzeOptions& options) {
  MusicAnalyzerConfig config;
  config.n_fft = options.n_fft;
  config.hop_length = options.hop_length;
  config.bpm_min = options.bpm_min;
  config.bpm_max = options.bpm_max;
  config.start_bpm = options.start_bpm;
  config.use_triads_only = options.use_triads_only != 0;
  config.use_hpss = options.use_hpss != 0;
  config.chroma_highpass_hz = options.chroma_high_pass_hz;
  config.use_bass_weighted = options.use_bass_weighted != 0;
  config.chroma_hop_multiplier = options.chroma_hop_multiplier;
  config.use_chord_hmm = options.use_chord_hmm != 0;
  config.use_chord_key_context = options.use_chord_key_context != 0;
  config.chord_hmm_beam_width = options.chord_hmm_beam_width;
  config.detect_chord_inversions = options.detect_chord_inversions != 0;
  config.adaptive_tempo = options.adaptive_tempo != 0;
  config.tempo_update_interval_beats = options.tempo_update_interval_beats;
  config.compute_tempo_curve = options.compute_tempo_curve != 0;
  config.meter_candidate_numerators.assign(
      options.meter_candidate_numerators,
      options.meter_candidate_numerators + options.meter_candidate_numerator_count);
  config.meter_denominator = options.meter_denominator;
  config.tuning = tuning_from_c(options.tuning_cents, options.tuning_auto);
  return config;
}

// The core config member names, as the options structs spell them.
const std::array<CName, 12> kMusicAnalyzeOptionNames{
    {{"n_fft", "options.n_fft"},
     {"hop_length", "options.hop_length"},
     {"bpm_min", "options.bpm_min"},
     {"bpm_max", "options.bpm_max"},
     {"start_bpm", "options.start_bpm"},
     {"chroma_highpass_hz", "options.chroma_high_pass_hz"},
     {"chroma_hop_multiplier", "options.chroma_hop_multiplier"},
     {"tuning", "options.tuning_cents"},
     {"chord_hmm_beam_width", "options.chord_hmm_beam_width"},
     {"tempo_update_interval_beats", "options.tempo_update_interval_beats"},
     {"meter_candidate_numerators", "options.meter_candidate_numerators"},
     {"meter_denominator", "options.meter_denominator"}}};

const std::array<CName, 6> kMeterOptionNames{
    {{"candidate_numerators", "options.candidate_numerators"},
     {"denominator", "options.denominator"},
     {"downbeat_weight", "options.downbeat_weight"},
     {"measure_weight", "options.measure_weight"},
     {"subdivision_weight", "options.subdivision_weight"},
     {"compound_subdivision_threshold", "options.compound_subdivision_threshold"}}};

}  // namespace

SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                const SonareMusicAnalyzeOptions* options, char** out_json) {
  SONARE_C_API_ENTRY;
  // Field rules live in validate_config, which MusicAnalyzer's constructor
  // applies below; this entry point only checks what the core cannot see (the
  // pointers, the layout version and the length of the flat candidate array).
  SONARE_C_ZERO_OUTS(out_json);
  if (const SonareError err =
          validate_audio_params("analyze_json", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SonareMusicAnalyzeOptions defaults;
  if (options == nullptr) {
    sonare_music_analyze_options_init(&defaults);
    options = &defaults;
  }
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(options->struct_size,
                                             sizeof(SonareMusicAnalyzeOptions), "options",
                                             "sonare_music_analyze_options_init");
        util::check::integer_in_range(options->meter_candidate_numerator_count, 0,
                                      SONARE_MAX_METER_CANDIDATE_NUMERATORS,
                                      "options.meter_candidate_numerator_count");
        with_c_names(kMusicAnalyzeOptionNames,
                     [&] { validate_config(music_analyzer_config_from_options(*options)); });
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_json);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        MusicAnalyzer analyzer(audio, music_analyzer_config_from_options(*options));
        const SonareAnalyzeProgressCallback callback = options->progress_callback;
        void* const user_data = options->progress_user_data;
        const SonareCancelCallback cancel_cb = options->cancel_callback;
        void* const cancel_user_data = options->cancel_user_data;
        if (callback != nullptr) {
          analyzer.set_progress_callback([callback, user_data](float progress, const char* stage) {
            callback(progress, stage, user_data);
          });
        }
        if (cancel_cb != nullptr) {
          analyzer.set_cancel_callback(
              [cancel_cb, cancel_user_data]() { return cancel_cb(cancel_user_data) != 0; });
          auto result = analyzer.analyze_cancellable();
          if (!result) return SONARE_ERROR_CANCELLED;
          *out_json = copy_string(analysis_result_to_json(*result));
        } else {
          AnalysisResult result = analyzer.analyze();
          *out_json = copy_string(analysis_result_to_json(result));
        }
        return SONARE_OK;
      });
}

void sonare_meter_options_init(SonareMeterOptions* options) {
  if (options == nullptr) return;
  const MeterConfig config;
  *options = {};
  options->struct_size = sizeof(*options);
  options->denominator = config.denominator;
  options->downbeat_weight = config.downbeat_weight;
  options->measure_weight = config.measure_weight;
  options->subdivision_weight = config.subdivision_weight;
  options->compound_subdivision_threshold = config.compound_subdivision_threshold;
  const size_t count = std::min(config.candidate_numerators.size(),
                                static_cast<size_t>(SONARE_MAX_METER_CANDIDATE_NUMERATORS));
  for (size_t i = 0; i < count; ++i) {
    options->candidate_numerators[i] = config.candidate_numerators[i];
  }
  options->candidate_numerator_count = static_cast<int>(count);
}

SonareError sonare_estimate_meter_json(const float* beat_times, const float* beat_strengths,
                                       size_t beat_count, const SonareMeterOptions* options,
                                       char** out_json) {
  SONARE_C_API_ENTRY;
  // Value rules live in validate_meter_config, which estimate_meter_from_beats
  // applies; this entry point only checks what the core cannot see (the
  // pointers and the length of the flat candidate array it never receives).
  SONARE_C_ZERO_OUTS(out_json);
  // A zero count with null pointers is an empty beat series, not a null-pointer
  // mistake: the core's own empty-series refusal answers it.
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(beat_times, beat_count);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(beat_strengths, beat_count);
  SONARE_C_REQUIRE_NONNULL(options);
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareMeterOptions),
                                             "options", "sonare_meter_options_init");
        util::check::integer_in_range(options->candidate_numerator_count, 0,
                                      SONARE_MAX_METER_CANDIDATE_NUMERATORS,
                                      "options.candidate_numerator_count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_json);

  SONARE_C_TRY
  MeterConfig config;
  config.candidate_numerators.assign(
      options->candidate_numerators,
      options->candidate_numerators + options->candidate_numerator_count);
  config.denominator = options->denominator;
  config.downbeat_weight = options->downbeat_weight;
  config.measure_weight = options->measure_weight;
  config.subdivision_weight = options->subdivision_weight;
  config.compound_subdivision_threshold = options->compound_subdivision_threshold;

  // Built only when there is something to copy: a (null, null) iterator pair is
  // not a valid empty range, and an empty series has to reach the core guard
  // rather than trip over its own construction on the way there.
  std::vector<float> times;
  std::vector<float> strengths;
  if (beat_count > 0) {
    times.assign(beat_times, beat_times + beat_count);
    strengths.assign(beat_strengths, beat_strengths + beat_count);
  }
  *out_json = copy_string(meter_result_to_json(with_c_names(
      kMeterOptionNames, [&] { return estimate_meter_from_beats(times, strengths, config); })));
  return SONARE_OK;
  SONARE_C_CATCH
}
