#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"
#include "util/parameter_checks.h"

namespace check = sonare::util::check;

#if defined(SONARE_WITH_ARRANGEMENT)
#include "analysis/music_analyzer.h"
#include "arrangement/harmonic_limits.h"
#include "core/resample.h"
#include "feature/cqt.h"
#include "mir/key_context.h"
#endif

// ============================================================================
// MIR
// ============================================================================

void sonare_project_tempo_options_init(SonareProjectTempoOptions* out) {
  if (out == nullptr) return;
  SonareProjectTempoOptions options = {};
  options.struct_size = sizeof(SonareProjectTempoOptions);
#if defined(SONARE_WITH_ARRANGEMENT)
  // Seeded from the two core configs the bridge runs on, so the defaults cannot
  // drift away from what the fixed-signature entry points do.
  const sonare::BeatConfig beat_config;
  const sonare::mir::TempoEstimatorConfig bridge_config;
  options.adaptive_tempo = beat_config.adaptive_tempo ? 1 : 0;
  options.tempo_update_interval_beats = beat_config.tempo_update_interval_beats;
  options.ramp_threshold = bridge_config.ramp_threshold;
  options.include_octave_candidates = bridge_config.include_octave_candidates ? 1 : 0;
#endif
  // Without arrangement the bridge these options configure is not in the build
  // and every entry point that would read them reports NOT_SUPPORTED, so the
  // zeroed struct is the honest answer rather than a hand-copied set of values
  // with nothing to keep them in step with the core.
  *out = options;
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

SonareProjectTempoCandidateKind tempo_candidate_kind(const char* label) noexcept {
  if (label != nullptr && std::strcmp(label, "half") == 0) return SONARE_TEMPO_CANDIDATE_HALF;
  if (label != nullptr && std::strcmp(label, "double") == 0) return SONARE_TEMPO_CANDIDATE_DOUBLE;
  return SONARE_TEMPO_CANDIDATE_PRIMARY;
}

void fill_tempo_candidate(const sonare::mir::TempoEstimate& estimate,
                          SonareProjectTempoCandidate* out) noexcept {
  *out = {};
  out->bpm = estimate.segments.empty() ? 0.0f : static_cast<float>(estimate.segments.front().bpm);
  out->confidence = estimate.confidence;
  out->kind = tempo_candidate_kind(estimate.label);
  out->time_signature_count = estimate.time_sigs.size();
  if (!estimate.time_sigs.empty()) {
    const auto& time_sig = estimate.time_sigs.front();
    out->first_time_signature = {time_sig.start_ppq, time_sig.time_sig.numerator,
                                 time_sig.time_sig.denominator};
  }
}

std::vector<sonare::mir::TempoEstimate> analyze_tempo_candidates(
    const float* audio, size_t len, int sample_rate, const SonareProjectTempoOptions& options) {
  sonare::BeatConfig beat_config;
  beat_config.adaptive_tempo = options.adaptive_tempo != 0;
  beat_config.tempo_update_interval_beats = options.tempo_update_interval_beats;
  sonare::mir::TempoEstimatorConfig bridge_config;
  bridge_config.ramp_threshold = options.ramp_threshold;
  bridge_config.include_octave_candidates = options.include_octave_candidates != 0;

  sonare::Audio wrapped = sonare::Audio::from_buffer(audio, len, sample_rate);
  sonare::BeatAnalyzer analyzer(wrapped, beat_config);
  return sonare::mir::estimate_tempo(sonare::mir::make_input_from_analyzer(analyzer),
                                     bridge_config);
}

// The values are checked rather than clamped.
void check_tempo_options(const SonareProjectTempoOptions& options) {
  sonare_c_detail::require_struct_size(options.struct_size, sizeof(SonareProjectTempoOptions),
                                       "options", "sonare_project_tempo_options_init");
  check::integer_within(options.tempo_update_interval_beats, sonare::kMusicCountDomain,
                        "options.tempo_update_interval_beats");
  check::at_least(options.ramp_threshold, 0.0, "options.ramp_threshold");
}

// The refusal for a candidate_index that names no usable tempo candidate.
void check_tempo_candidate(const std::vector<sonare::mir::TempoEstimate>& estimates,
                           size_t candidate_index) {
  if (candidate_index >= estimates.size() || estimates[candidate_index].segments.empty()) {
    check::refuse("candidate_index", "must be the index of a tempo candidate of this take");
  }
}

}  // namespace
#endif

SonareError sonare_project_analyze_tempo(const SonareProject* project, const float* audio,
                                         size_t len, int sample_rate,
                                         const SonareProjectTempoOptions* options,
                                         SonareProjectTempoCandidate* candidates, size_t capacity,
                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_count) *out_count = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SonareError err =
      validate_audio_params("project_analyze_tempo", "audio", audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SonareProjectTempoOptions resolved;
  if (options) {
    resolved = *options;
  } else {
    sonare_project_tempo_options_init(&resolved);
  }
  if (err = run_validation([&] { check_tempo_options(resolved); }); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(candidates, capacity);
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate, resolved);
  if (estimates.empty()) return SONARE_ERROR_INVALID_STATE;
  if (out_count) *out_count = estimates.size();
  const size_t emitted = std::min(capacity, estimates.size());
  for (size_t i = 0; i < emitted; ++i) fill_tempo_candidate(estimates[i], &candidates[i]);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, options, candidates, capacity,
                              out_count);
#endif
}

SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, const SonareProjectTempoOptions* options,
                                      size_t candidate_index, uint8_t apply_time_signatures,
                                      float* out_bpm) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bpm) *out_bpm = 0.0f;
  SONARE_C_REQUIRE_NONNULL(project);
  SonareError err = validate_audio_params("project_auto_tempo", "audio", audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SonareProjectTempoOptions resolved;
  if (options) {
    resolved = *options;
  } else {
    sonare_project_tempo_options_init(&resolved);
  }
  if (err = run_validation([&] { check_tempo_options(resolved); }); err != SONARE_OK) return err;
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate, resolved);
  check_tempo_candidate(estimates, candidate_index);
  const sonare::mir::TempoEstimate& selected = estimates[candidate_index];
  auto command = std::make_unique<arr::SetTempoSegment>(selected.segments);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (apply_time_signatures != 0 && !selected.time_sigs.empty()) {
    auto time_signature_command =
        std::make_unique<arr::SetTimeSignatureSegment>(selected.time_sigs);
    if (!project->history.apply(std::move(time_signature_command)))
      return SONARE_ERROR_INVALID_STATE;
  }
  if (out_bpm) *out_bpm = static_cast<float>(selected.segments.front().bpm);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_bpm) *out_bpm = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, options, candidate_index,
                              apply_time_signatures, out_bpm);
#endif
}

void sonare_project_snap_to_grid_options_init(SonareProjectSnapToGridOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->division = 1;
}

SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        const SonareProjectSnapToGridOptions* options,
                                        double* out_ppq) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_ppq) *out_ppq = ppq;
  SONARE_C_REQUIRE_NONNULL(project);
  int division = 1;
  if (options != nullptr) {
    if (const SonareError err = run_validation([&] {
          sonare_c_detail::require_struct_size(options->struct_size,
                                               sizeof(SonareProjectSnapToGridOptions), "options",
                                               "sonare_project_snap_to_grid_options_init");
        });
        err != SONARE_OK) {
      return err;
    }
    division = options->division;
  }
  if (const SonareError err = run_validation([&] {
        check::at_least(ppq, 0.0, "ppq");
        check::in_range(strength, 0.0, 1.0, "strength");
        check::integer_within(division, kSnapDivisionDomain, "division");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_ppq);
  SONARE_C_TRY
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const sonare::mir::SnapGrid grid = sonare::mir::make_grid(map, ppq);
  if (division == 0) {
    *out_ppq = sonare::mir::snap_to_bar(grid, ppq, strength);
  } else if (division == 1) {
    *out_ppq = sonare::mir::snap_to_beat(grid, ppq, strength);
  } else {
    *out_ppq = sonare::mir::snap_to_subdivision(grid, ppq, division, strength);
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_ppq) *out_ppq = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, ppq, strength, options, out_ppq);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// Refuses a pitch class that is neither 0..11 nor the unknown marker.
void check_pitch_class(uint32_t value, const std::string& subject) {
  if (value > 11u && value != arr::kUnknownPitchClass) {
    check::refuse(subject.c_str(),
                  "must be an integer in [0, 11] or " +
                      std::to_string(static_cast<unsigned>(arr::kUnknownPitchClass)));
  }
}

bool key_mode_from_c(uint32_t value, arr::KeyMode* out) noexcept {
  return ordinal_to_enum(value, 0, static_cast<int>(arr::KeyMode::kLocrian), out);
}

bool chord_quality_from_c(uint32_t value, arr::ChordQuality* out) noexcept {
  return ordinal_to_enum(value, 0, static_cast<int>(arr::ChordQuality::kSuspended), out);
}

// Refuses a span that does not start at a finite, non-negative position and end after it.
void check_ppq_span(double start, double end, const std::string& start_subject,
                    const std::string& end_subject) {
  check::at_least(start, 0.0, start_subject.c_str());
  check::finite(end, end_subject.c_str());
  if (!(end > start)) {
    check::refuse_relation({end_subject, start_subject}, "{0} must be greater than {1}");
  }
}

}  // namespace
#endif

SonareError sonare_project_annotate_keys(SonareProject* project,
                                         const SonareProjectKeySegment* keys, size_t count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(keys, count);
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), kAnnotationCountDomain, "count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  arr::ProjectAnnotation annotation = project->history.project().annotation();
  annotation.keys.clear();
  annotation.keys.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const SonareProjectKeySegment& in = keys[i];
    arr::KeySegment out;
    const auto subject = [i](const char* field) {
      return check::indexed_subject("keys", i, field);
    };
    check_ppq_span(in.start_ppq, in.end_ppq, subject("start_ppq"), subject("end_ppq"));
    check_pitch_class(in.tonic_pc, subject("tonic_pc"));
    if (!key_mode_from_c(in.mode, &out.mode)) {
      check::refuse(subject("mode").c_str(), sonare::util::refusal::kDeclaredValuePredicate);
    }
    out.start_ppq = in.start_ppq;
    out.end_ppq = in.end_ppq;
    out.tonic_pc = static_cast<uint8_t>(in.tonic_pc);
    annotation.keys.push_back(out);
  }
  auto command = std::make_unique<arr::SetAnnotation>(std::move(annotation));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, keys, count);
#endif
}

SonareError sonare_project_annotate_chords(SonareProject* project,
                                           const SonareProjectChordSymbol* chords, size_t count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(chords, count);
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), kAnnotationCountDomain, "count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  std::vector<arr::ChordSymbol> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const SonareProjectChordSymbol& in = chords[i];
    arr::ChordSymbol chord;
    const auto subject = [i](const char* field) {
      return check::indexed_subject("chords", i, field);
    };
    check_ppq_span(in.start_ppq, in.end_ppq, subject("start_ppq"), subject("end_ppq"));
    check_pitch_class(in.root_pc, subject("root_pc"));
    if (!chord_quality_from_c(in.quality, &chord.quality)) {
      check::refuse(subject("quality").c_str(), sonare::util::refusal::kDeclaredValuePredicate);
    }
    check::non_null_when_count(in.extensions, in.extension_count, subject("extensions").c_str(),
                               subject("extension_count").c_str());
    check::integer_in_range(static_cast<long long>(in.extension_count), 0,
                            static_cast<long long>(arr::kMaxChordExtensions),
                            subject("extension_count").c_str());
    check_pitch_class(in.slash_bass_pc, subject("slash_bass_pc"));
    chord.start_ppq = in.start_ppq;
    chord.end_ppq = in.end_ppq;
    chord.root_pc = static_cast<uint8_t>(in.root_pc);
    chord.slash_bass_pc = static_cast<uint8_t>(in.slash_bass_pc);
    chord.modulation_boundary = in.modulation_boundary != 0;
    if (in.extension_count > 0) {
      chord.extensions.assign(in.extensions, in.extensions + in.extension_count);
    }
    if (in.roman_numeral != nullptr) {
      chord.roman_numeral = in.roman_numeral;
    }
    out.push_back(std::move(chord));
  }
  auto command = std::make_unique<arr::SetHarmonySegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, chords, count);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
// The analysis ordinals convert onto the core enums ordinal for ordinal below,
// so the range checks in those conversions are only sound while the two agree.
static_assert(SONARE_CHORD_QUALITY_COUNT == sonare::kChordQualityCount,
              "SonareChordQuality count drift");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7s9) == SONARE_CHORD_DOMINANT7_SHARP9,
              "SonareChordQuality ordinal drift");
static_assert(static_cast<int>(sonare::Mode::Locrian) == SONARE_MODE_LOCRIAN,
              "SonareMode ordinal drift");
#endif

SonareError sonare_chord_symbol_from_analysis(SonarePitchClass root, SonareChordQuality quality,
                                              SonarePitchClass bass, uint32_t* out_root_pc,
                                              uint32_t* out_quality, uint8_t* out_extensions,
                                              size_t extension_capacity,
                                              size_t* out_extension_count,
                                              uint32_t* out_slash_bass_pc) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_root_pc) *out_root_pc = arr::kUnknownPitchClass;
  if (out_quality) *out_quality = static_cast<uint32_t>(arr::ChordQuality::kUnknown);
  if (out_extension_count) *out_extension_count = 0;
  if (out_slash_bass_pc) *out_slash_bass_pc = arr::kUnknownPitchClass;
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(root), kChordRootPitchClassDomain, "root");
        check::integer_in_range(static_cast<long long>(quality), 0, SONARE_CHORD_QUALITY_COUNT - 1,
                                "quality");
        check::integer_in_range(static_cast<long long>(bass), SONARE_PITCH_C, SONARE_PITCH_B,
                                "bass");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_root_pc);
  SONARE_C_REQUIRE_NONNULL(out_quality);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out_extensions, extension_capacity);
  SONARE_C_REQUIRE_NONNULL(out_extension_count);
  SONARE_C_REQUIRE_NONNULL(out_slash_bass_pc);
  SONARE_C_TRY
  const auto core_quality = static_cast<sonare::ChordQuality>(quality);
  const sonare::mir::MappedQuality mapped = sonare::mir::map_chord_quality(core_quality);
  *out_extension_count = mapped.extensions.size();
  if (const SonareError err = run_validation([&] {
        check::integer_at_least<size_t>(extension_capacity,
                                        static_cast<long long>(mapped.extensions.size()),
                                        "extension_capacity");
      });
      err != SONARE_OK) {
    return err;
  }
  std::copy(mapped.extensions.begin(), mapped.extensions.end(), out_extensions);
  // Root kept even for an unknown quality, and a bass equal to the root is no
  // slash bass: both are the rules build_harmonic_timeline applies.
  *out_root_pc = static_cast<uint32_t>(root);
  *out_quality = static_cast<uint32_t>(mapped.quality);
  *out_slash_bass_pc = bass == root ? arr::kUnknownPitchClass : static_cast<uint32_t>(bass);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_root_pc) *out_root_pc = {};
  if (out_quality) *out_quality = {};
  if (out_extension_count) *out_extension_count = {};
  if (out_slash_bass_pc) *out_slash_bass_pc = {};
  SONARE_C_STUB_NOT_SUPPORTED(root, quality, bass, out_root_pc, out_quality, out_extensions,
                              extension_capacity, out_extension_count, out_slash_bass_pc);
#endif
}

SonareError sonare_key_mode_from_analysis(SonareMode mode, uint32_t* out_mode) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_mode) *out_mode = static_cast<uint32_t>(arr::KeyMode::kUnknown);
  // The core maps an out-of-range mode to unknown without a word, so the
  // refusal has to happen here.
  if (const SonareError err = run_validation([&] {
        check::integer_in_range(static_cast<long long>(mode), SONARE_MODE_MAJOR,
                                SONARE_MODE_LOCRIAN, "mode");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_mode);
  SONARE_C_TRY
  *out_mode = static_cast<uint32_t>(sonare::mir::map_key_mode(static_cast<sonare::Mode>(mode)));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_mode) *out_mode = {};
  SONARE_C_STUB_NOT_SUPPORTED(mode, out_mode);
#endif
}

// ============================================================================
// Take alignment
// ============================================================================

void sonare_take_align_config_init(SonareTakeAlignConfig* config) {
  if (config == nullptr) return;
  *config = SonareTakeAlignConfig{};
  config->struct_size = sizeof(SonareTakeAlignConfig);
#if defined(SONARE_WITH_ARRANGEMENT)
  const sonare::mir::ChromaDtwConfig defaults;
  config->hop_length = static_cast<size_t>(defaults.hop_length);
  config->bins_per_octave = defaults.bins_per_octave;
#endif
}

SonareError sonare_align_take_to_reference(const float* reference, size_t reference_len,
                                           int reference_sample_rate, const float* take,
                                           size_t take_len, int take_sample_rate,
                                           const SonareTakeAlignConfig* config,
                                           SonareProjectWarpAnchor** out_anchors, size_t* out_count,
                                           SonareTakeAlignment* out_alignment) {
  SONARE_C_API_ENTRY;
  if (out_anchors != nullptr) *out_anchors = nullptr;
  if (out_count != nullptr) *out_count = 0;
  if (out_alignment != nullptr) *out_alignment = SonareTakeAlignment{};
#if defined(SONARE_WITH_ARRANGEMENT)
  if (const SonareError err = validate_audio_params(
          "align_take_to_reference", "reference", reference, reference_len, reference_sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = validate_audio_params("align_take_to_reference", "take", take,
                                                    take_len, take_sample_rate);
      err != SONARE_OK) {
    return err;
  }
  const bool resample_take = take_sample_rate != reference_sample_rate;
  // Sized before anything is copied, so a rate ratio that would blow the take up
  // past the buffer cap is refused rather than allocated.
  size_t resampled_take_len = take_len;
  if (resample_take &&
      !sonare::numeric::checked_converted_count(take_len, take_sample_rate, reference_sample_rate,
                                                kMaxBufferSize, &resampled_take_len)) {
    return run_validation([] {
      check::refuse_relation({"take_len", "take_sample_rate", "reference_sample_rate"},
                             "{0} resampled from {1} to {2} must not exceed the buffer size limit");
    });
  }
  SonareTakeAlignConfig resolved;
  if (config != nullptr) {
    resolved = *config;
  } else {
    sonare_take_align_config_init(&resolved);
  }
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(resolved.struct_size, sizeof(SonareTakeAlignConfig),
                                             "config", "sonare_take_align_config_init");
        // The chroma is a constant-Q one, so its hop takes the transform's bound.
        check::integer_within(resolved.hop_length, sonare::kCqtHopLengthDomain,
                              "config.hop_length");
        // Chroma folds the grid into 12 classes, so a grid it cannot fold is refused by its member.
        check::integer_within(resolved.bins_per_octave, sonare::kCqtCountDomain,
                              "config.bins_per_octave");
        if (resolved.bins_per_octave % 12 != 0) {
          check::refuse("config.bins_per_octave", "must be a multiple of 12");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_anchors);
  SONARE_C_REQUIRE_NONNULL(out_count);

  sonare::mir::ChromaDtwConfig dtw_config;
  dtw_config.hop_length = static_cast<int>(resolved.hop_length);
  dtw_config.bins_per_octave = resolved.bins_per_octave;

  SONARE_C_TRY
  const Audio reference_audio = Audio::from_vector(
      std::vector<float>(reference, reference + reference_len), reference_sample_rate);
  // The take goes onto the reference's grid, so one hop means the same span of
  // time in both signals and both anchor axes come back in reference-rate samples.
  const Audio take_audio = Audio::from_vector(
      resample_take ? sonare::resample(take, take_len, take_sample_rate, reference_sample_rate)
                    : std::vector<float>(take, take + take_len),
      reference_sample_rate);
  // Argument order inverted on purpose, and this is the whole reason the entry
  // point exists rather than the core call being exposed directly: the core names
  // the signal that lands on the SOURCE axis its `reference`, while a clip needs
  // its own recording there. Passing the take as the core's reference is what puts
  // the reference timeline on the warp axis.
  const sonare::mir::ChromaDtwResult aligned = sonare::detail::with_derived_cqt_grid(
      "config.bins_per_octave", "reference_sample_rate",
      [&] { return sonare::mir::chroma_dtw_align(take_audio, reference_audio, dtw_config); });

  const std::vector<sonare::mir::WarpAnchor> anchors =
      sonare::mir::strictly_increasing_anchors(aligned.anchors);
  if (out_alignment != nullptr) {
    out_alignment->mean_residual_frames = aligned.mean_residual_frames;
    // Named for the arguments this entry point took, not for the ones it passed
    // on, so the two counts describe the signals the caller handed over.
    out_alignment->reference_frames = aligned.target_frames;
    out_alignment->take_frames = aligned.reference_frames;
  }
  // The reduction hands back its input when it cannot produce two distinct
  // anchors, so the count is checked here rather than assumed from the alignment
  // having succeeded.
  bool usable = anchors.size() >= 2;
  for (const sonare::mir::WarpAnchor& anchor : anchors) {
    usable = usable && std::isfinite(anchor.warp_sample) && std::isfinite(anchor.source_sample);
  }
  if (!usable) {
    check::refuse_relation({"reference", "take"},
                           "{0} and {1} must produce at least two distinct alignment anchors");
  }

  auto rows = std::make_unique<SonareProjectWarpAnchor[]>(anchors.size());
  for (size_t i = 0; i < anchors.size(); ++i) {
    rows[i].warp_sample = anchors[i].warp_sample;
    rows[i].source_sample = anchors[i].source_sample;
  }
  *out_count = anchors.size();
  *out_anchors = rows.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_anchors) *out_anchors = nullptr;
  if (out_count) *out_count = 0;
  SONARE_C_STUB_NOT_SUPPORTED(reference, reference_len, reference_sample_rate, take, take_len,
                              take_sample_rate, config, out_anchors, out_count, out_alignment);
#endif
}

void sonare_free_project_warp_anchors(SonareProjectWarpAnchor* anchors) { delete[] anchors; }

// ============================================================================
// Memory management
// ============================================================================

void sonare_free_bytes(uint8_t* ptr) { delete[] ptr; }
