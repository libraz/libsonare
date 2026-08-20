#include "c_api/project_internal.h"

// ============================================================================
// MIR
// ============================================================================

SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, float* out_bpm) {
  return sonare_project_auto_tempo_ex(project, audio, len, sample_rate, 0, 0, out_bpm);
}

SonareProjectTempoOptions sonare_project_tempo_options_default(void) {
  SonareProjectTempoOptions options = {};
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
  return options;
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
  out->time_signature_count = static_cast<uint32_t>(estimate.time_sigs.size());
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

std::vector<sonare::mir::TempoEstimate> analyze_tempo_candidates(const float* audio, size_t len,
                                                                 int sample_rate) {
  return analyze_tempo_candidates(audio, len, sample_rate, sonare_project_tempo_options_default());
}

// A caller who zeroed the struct instead of seeding it would silently get a
// whole-take single segment and an interval the local estimate cannot use, so
// the values are checked rather than clamped.
bool tempo_options_valid(const SonareProjectTempoOptions& options) noexcept {
  return options.tempo_update_interval_beats > 0 && std::isfinite(options.ramp_threshold) &&
         options.ramp_threshold >= 0.0f;
}

}  // namespace
#endif

SonareError sonare_project_analyze_tempo(const SonareProject* project, const float* audio,
                                         size_t len, int sample_rate,
                                         SonareProjectTempoCandidate* candidates, size_t capacity,
                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_count) *out_count = 0;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  if (capacity > 0 && !candidates) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate);
  if (estimates.empty()) return SONARE_ERROR_INVALID_STATE;
  if (out_count) *out_count = estimates.size();
  const size_t emitted = std::min(capacity, estimates.size());
  for (size_t i = 0; i < emitted; ++i) fill_tempo_candidate(estimates[i], &candidates[i]);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, candidates, capacity, out_count);
#endif
}

SonareError sonare_project_analyze_tempo_with_options(const SonareProject* project,
                                                      const float* audio, size_t len,
                                                      int sample_rate,
                                                      const SonareProjectTempoOptions* options,
                                                      SonareProjectTempoCandidate* candidates,
                                                      size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_count) *out_count = 0;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  if (capacity > 0 && !candidates) return SONARE_ERROR_INVALID_PARAMETER;
  const SonareProjectTempoOptions resolved =
      options ? *options : sonare_project_tempo_options_default();
  if (!tempo_options_valid(resolved)) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
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
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, options, candidates, capacity,
                              out_count);
#endif
}

SonareError sonare_project_auto_tempo_with_options(SonareProject* project, const float* audio,
                                                   size_t len, int sample_rate,
                                                   const SonareProjectTempoOptions* options,
                                                   size_t candidate_index,
                                                   uint8_t apply_time_signatures, float* out_bpm) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bpm) *out_bpm = 0.0f;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  const SonareProjectTempoOptions resolved =
      options ? *options : sonare_project_tempo_options_default();
  if (!tempo_options_valid(resolved)) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate, resolved);
  if (candidate_index >= estimates.size() || estimates[candidate_index].segments.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, options, candidate_index,
                              apply_time_signatures, out_bpm);
#endif
}

SonareError sonare_project_auto_tempo_ex(SonareProject* project, const float* audio, size_t len,
                                         int sample_rate, size_t candidate_index,
                                         uint8_t apply_time_signatures, float* out_bpm) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bpm) *out_bpm = 0.0f;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  const std::vector<sonare::mir::TempoEstimate> estimates =
      analyze_tempo_candidates(audio, len, sample_rate);
  if (candidate_index >= estimates.size() || estimates[candidate_index].segments.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, candidate_index,
                              apply_time_signatures, out_bpm);
#endif
}

SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        double* out_ppq) {
  return sonare_project_snap_to_grid_ex(project, ppq, strength, 1, out_ppq);
}

SonareError sonare_project_snap_to_grid_ex(const SonareProject* project, double ppq,
                                           double strength, int division, double* out_ppq) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_ppq) *out_ppq = ppq;
  if (!project || !out_ppq || !finite_non_negative(ppq) || !std::isfinite(strength) ||
      strength < 0.0 || strength > 1.0 || division < 0 || division > 1024) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  SONARE_C_STUB_NOT_SUPPORTED(project, ppq, strength, division, out_ppq);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

bool valid_pitch_class(uint32_t value) noexcept { return value <= 11u || value == 255u; }

bool valid_key_mode(uint32_t value) noexcept {
  return value <= static_cast<uint32_t>(arr::KeyMode::kLocrian);
}

bool valid_chord_quality(uint32_t value) noexcept {
  return value <= static_cast<uint32_t>(arr::ChordQuality::kSuspended);
}

bool valid_ppq_span(double start, double end) noexcept {
  return finite_non_negative(start) && std::isfinite(end) && end > start;
}

}  // namespace
#endif

SonareError sonare_project_annotate_keys(SonareProject* project,
                                         const SonareProjectKeySegment* keys, size_t count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || (count > 0 && !keys) || count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::ProjectAnnotation annotation = project->history.project().annotation();
  annotation.keys.clear();
  annotation.keys.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const SonareProjectKeySegment& in = keys[i];
    if (!valid_ppq_span(in.start_ppq, in.end_ppq) || !valid_pitch_class(in.tonic_pc) ||
        !valid_key_mode(in.mode)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    arr::KeySegment out;
    out.start_ppq = in.start_ppq;
    out.end_ppq = in.end_ppq;
    out.tonic_pc = static_cast<uint8_t>(in.tonic_pc);
    out.mode = static_cast<arr::KeyMode>(in.mode);
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
  if (!project || (count > 0 && !chords) || count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<arr::ChordSymbol> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const SonareProjectChordSymbol& in = chords[i];
    if (!valid_ppq_span(in.start_ppq, in.end_ppq) || !valid_pitch_class(in.root_pc) ||
        !valid_chord_quality(in.quality) || !valid_pitch_class(in.slash_bass_pc) ||
        (in.extension_count > 0 && !in.extensions) || in.extension_count > 32) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    arr::ChordSymbol chord;
    chord.start_ppq = in.start_ppq;
    chord.end_ppq = in.end_ppq;
    chord.root_pc = static_cast<uint8_t>(in.root_pc);
    chord.quality = static_cast<arr::ChordQuality>(in.quality);
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

// ============================================================================
// Memory management
// ============================================================================

void sonare_free_bytes(uint8_t* ptr) { delete[] ptr; }
