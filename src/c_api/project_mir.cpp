#include <algorithm>
#include <cctype>

#include "analysis/music_analyzer.h"
#include "c_api/ppq_domains.h"
#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"
#include "mir/tempo_estimator_bridge.h"
#include "util/parameter_checks.h"

namespace check = sonare::util::check;

#if defined(SONARE_WITH_ARRANGEMENT)
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
  // Seed from the core configs so defaults remain the same with or without the
  // arrangement entry points in the build.
  const sonare::BeatConfig beat_config;
  const sonare::mir::TempoEstimatorConfig bridge_config;
  options.adaptive_tempo = beat_config.adaptive_tempo ? 1 : 0;
  options.tempo_update_interval_beats = beat_config.tempo_update_interval_beats;
  options.ramp_threshold = bridge_config.ramp_threshold;
  options.include_octave_candidates = bridge_config.include_octave_candidates ? 1 : 0;
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
  return sonare::mir::estimate_tempo_from_audio(wrapped, beat_config, bridge_config);
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
  SonareError err = validate_audio_params("audio", audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SonareProjectTempoOptions defaults{};
  if (options == nullptr) {
    sonare_project_tempo_options_init(&defaults);
    options = &defaults;
  }
  if (err = run_validation([&] { check_tempo_options(*options); }); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(candidates, capacity);
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate, *options);
  if (estimates.empty()) return SONARE_ERROR_INVALID_STATE;
  if (out_count) *out_count = estimates.size();
  const size_t emitted = std::min(capacity, estimates.size());
  for (size_t i = 0; i < emitted; ++i) fill_tempo_candidate(estimates[i], &candidates[i]);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 audio, len, sample_rate, options, candidates,
                                                 capacity, out_count);
#endif
}

SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, const SonareProjectTempoOptions* options,
                                      size_t candidate_index, int32_t apply_time_signatures,
                                      SonareProjectAutoTempoResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SonareError err = validate_audio_params("audio", audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SonareProjectTempoOptions defaults{};
  const SonareProjectTempoOptions* supplied_options = options;
  if (supplied_options == nullptr) {
    sonare_project_tempo_options_init(&defaults);
    supplied_options = &defaults;
  }
  if (err = run_validation([&] { check_tempo_options(*supplied_options); }); err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate, *supplied_options);
  check_tempo_candidate(estimates, candidate_index);
  SONARE_C_REQUIRE_NONNULL(out);
  const sonare::mir::TempoEstimate& selected = estimates[candidate_index];
  SonareProjectAutoTempoResult result{};
  result.bpm = static_cast<float>(selected.segments.front().bpm);
  result.bpm_confidence = selected.confidence;
  result.time_signature = {selected.meter.numerator, selected.meter.denominator,
                           selected.meter.confidence};

  // Either way detected beat k lands on a beat line; the meter decides only which map carries it.
  const bool with_meter = apply_time_signatures != 0 && !selected.time_sigs.empty();
  std::vector<arr::EditCommandPtr> commands;
  commands.push_back(std::make_unique<arr::SetTempoSegment>(
      with_meter ? selected.segments : selected.unmetered_segments));
  if (with_meter) {
    commands.push_back(std::make_unique<arr::SetTimeSignatureSegment>(selected.time_sigs));
  }
  if (!project->history.apply_transaction(std::move(commands))) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_STATE,
                                    "the tempo candidate could not be applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  *out = result;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 audio, len, sample_rate, options, candidate_index,
                                                 apply_time_signatures, out);
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
  SONARE_C_ZERO_OUTS(out_ppq);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (const SonareError err =
          run_validation([&] { sonare_c_detail::check_snap_position_and_strength(ppq, strength); });
      err != SONARE_OK) {
    return err;
  }
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
  if (const SonareError err = run_validation(
          [&] { check::integer_within(division, kSnapDivisionDomain, "options.division"); });
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 ppq, strength, options, out_ppq);
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

// Refuses a span whose bounds are not timeline positions or that does not end after it starts.
void check_ppq_span(double start, double end, const std::string& start_subject,
                    const std::string& end_subject) {
  check::within(start, sonare_c_detail::kPublicPpqDomain, start_subject.c_str());
  check::within(end, sonare_c_detail::kPublicPpqDomain, end_subject.c_str());
  if (!(end > start)) {
    check::refuse_relation({end_subject, start_subject}, "{0} must be greater than {1}");
  }
}

void check_section_label(const char* label, const std::string& subject) {
  check::non_null(label, subject.c_str());
  const std::string value(label);
  if (std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); })) {
    check::refuse(subject.c_str(), "must not be blank");
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 keys, count);
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
      check::non_empty(in.roman_numeral, subject("roman_numeral").c_str());
      chord.roman_numeral = in.roman_numeral;
    }
    out.push_back(std::move(chord));
  }
  auto command = std::make_unique<arr::SetHarmonySegment>(std::move(out));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 chords, count);
#endif
}

SonareError sonare_project_annotate_sections(SonareProject* project,
                                             const SonareProjectSection* sections, size_t count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(sections, count);
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), kAnnotationCountDomain, "count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  arr::ProjectAnnotation annotation = project->history.project().annotation();
  annotation.sections.clear();
  annotation.sections.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const SonareProjectSection& in = sections[i];
    const auto subject = [i](const char* field) {
      return check::indexed_subject("sections", i, field);
    };
    check_ppq_span(in.start_ppq, in.end_ppq, subject("start_ppq"), subject("end_ppq"));
    check_section_label(in.label, subject("label"));
    annotation.sections.push_back({in.start_ppq, in.end_ppq, in.label});
  }
  auto command = std::make_unique<arr::SetAnnotation>(std::move(annotation));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 sections, count);
#endif
}

void sonare_free_project_key_segment_list(SonareProjectKeySegmentList* list) {
  if (list == nullptr) return;
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_key_annotations(const SonareProject* project,
                                           SonareProjectKeySegmentList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& keys = project->history.project().annotation().keys;
  if (keys.empty()) return SONARE_OK;

  sonare_c_detail::StagedOutput<SonareProjectKeySegmentList, &sonare_free_project_key_segment_list>
      staged;
  staged->items = new SonareProjectKeySegment[keys.size()]{};
  staged->count = keys.size();
  for (size_t i = 0; i < keys.size(); ++i) {
    const auto& key = keys[i];
    staged->items[i] = {key.start_ppq, key.end_ppq, key.tonic_pc, static_cast<uint32_t>(key.mode)};
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

void sonare_free_project_chord_symbol_list(SonareProjectChordSymbolList* list) {
  if (list == nullptr) return;
  for (size_t i = 0; i < list->count; ++i) {
    delete[] list->items[i].extensions;
    delete[] list->items[i].roman_numeral;
  }
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_chord_annotations(const SonareProject* project,
                                             SonareProjectChordSymbolList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& chords = project->history.project().annotation().chords;
  if (chords.empty()) return SONARE_OK;

  sonare_c_detail::StagedOutput<SonareProjectChordSymbolList,
                                &sonare_free_project_chord_symbol_list>
      staged;
  staged->items = new SonareProjectChordSymbol[chords.size()]{};
  staged->count = chords.size();
  for (size_t i = 0; i < chords.size(); ++i) {
    const auto& chord = chords[i];
    auto& item = staged->items[i];
    item.start_ppq = chord.start_ppq;
    item.end_ppq = chord.end_ppq;
    item.root_pc = chord.root_pc;
    item.quality = static_cast<uint32_t>(chord.quality);
    item.extensions = sonare_c_detail::copy_vector(chord.extensions);
    item.extension_count = chord.extensions.size();
    item.slash_bass_pc = chord.slash_bass_pc;
    if (!chord.roman_numeral.empty()) {
      item.roman_numeral = sonare_c_detail::copy_string(chord.roman_numeral);
    }
    item.modulation_boundary = chord.modulation_boundary;
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

void sonare_free_project_section_list(SonareProjectSectionList* list) {
  if (list == nullptr) return;
  for (size_t i = 0; i < list->count; ++i) {
    delete[] list->items[i].label;
  }
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_section_annotations(const SonareProject* project,
                                               SonareProjectSectionList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& sections = project->history.project().annotation().sections;
  if (sections.empty()) return SONARE_OK;

  sonare_c_detail::StagedOutput<SonareProjectSectionList, &sonare_free_project_section_list> staged;
  staged->items = new SonareProjectSection[sections.size()]{};
  staged->count = sections.size();
  for (size_t i = 0; i < sections.size(); ++i) {
    const auto& section = sections[i];
    staged->items[i] = {section.start_ppq, section.end_ppq,
                        sonare_c_detail::copy_string(section.label)};
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
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
  return sonare_c_detail::refuse_not_compiled_in(
      sonare_c_detail::Feature::kArrangement, root, quality, bass, out_root_pc, out_quality,
      out_extensions, extension_capacity, out_extension_count, out_slash_bass_pc);
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, mode,
                                                 out_mode);
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

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {
const std::array<CName, 1> kReferenceRateNames{{{"sample_rate", "reference_sample_rate"}}};
const std::array<CName, 1> kTakeRateNames{{{"sample_rate", "take_sample_rate"}}};
}  // namespace
#endif

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
  if (const SonareError err = run_validation([&] {
        with_c_names(kReferenceRateNames, [&] {
          sonare::validate_offline_audio_input("reference", reference, reference_len,
                                               reference_sample_rate);
        });
      });
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        with_c_names(kTakeRateNames, [&] {
          sonare::validate_offline_audio_input("take", take, take_len, take_sample_rate);
        });
      });
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
  SonareTakeAlignment staged_alignment{};
  staged_alignment.mean_residual_frames = aligned.mean_residual_frames;
  // Name counts for the arguments this entry point took, not for those it passed
  // to the core, so they describe the signals the caller handed over.
  staged_alignment.reference_frames = aligned.target_frames;
  staged_alignment.take_frames = aligned.reference_frames;
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
  if (out_alignment != nullptr) *out_alignment = staged_alignment;
  *out_count = anchors.size();
  *out_anchors = rows.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_anchors) *out_anchors = nullptr;
  if (out_count) *out_count = 0;
  return sonare_c_detail::refuse_not_compiled_in(
      sonare_c_detail::Feature::kArrangement, reference, reference_len, reference_sample_rate, take,
      take_len, take_sample_rate, config, out_anchors, out_count, out_alignment);
#endif
}

void sonare_free_project_warp_anchors(SonareProjectWarpAnchor* anchors) { delete[] anchors; }
