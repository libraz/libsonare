#include "c_api/project_internal.h"

// ============================================================================
// MIR
// ============================================================================

SonareError sonare_project_auto_tempo(SonareProject* project, const float* audio, size_t len,
                                      int sample_rate, float* out_bpm) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bpm) *out_bpm = 0.0f;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(audio, len, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  sonare::Audio wrapped = sonare::Audio::from_buffer(audio, len, sample_rate);
  sonare::BeatAnalyzer analyzer(wrapped);
  sonare::mir::BeatAnalysisInput input = sonare::mir::make_input_from_analyzer(analyzer);
  std::vector<sonare::mir::TempoEstimate> estimates = sonare::mir::estimate_tempo(input);
  if (estimates.empty() || estimates.front().segments.empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  const sonare::mir::TempoEstimate& primary = estimates.front();
  auto command = std::make_unique<arr::SetTempoSegment>(primary.segments);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_bpm) *out_bpm = static_cast<float>(primary.segments.front().bpm);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, audio, len, sample_rate, out_bpm);
#endif
}

SonareError sonare_project_snap_to_grid(const SonareProject* project, double ppq, double strength,
                                        double* out_ppq) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_ppq) *out_ppq = ppq;
  if (!project || !out_ppq || !finite_non_negative(ppq) || !std::isfinite(strength) ||
      strength < 0.0 || strength > 1.0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  const sonare::mir::SnapGrid grid = sonare::mir::make_grid(map, ppq);
  *out_ppq = sonare::mir::snap_to_beat(grid, ppq, strength);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, ppq, strength, out_ppq);
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
