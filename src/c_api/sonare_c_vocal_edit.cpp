#include <sonare/sonare_c_vocal_edit.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <limits>
#include <memory>
#include <string>

#include "feature/pitch.h"
#include "sonare_c_internal.h"
#include "util/constants.h"

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "editing/vocal_edit/state_codec.h"
namespace vocal = sonare::editing::vocal_edit;
struct SonareVocalEditSession {
  vocal::VocalEditSession value;
};
struct SonareVocalEditDraft {
  std::unique_ptr<vocal::VocalEditDraft> value;
};
struct SonareVocalRenderSnapshot {
  std::shared_ptr<const vocal::VocalRenderSnapshot> value;
};
struct SonareVocalRenderJob {
  std::unique_ptr<vocal::VocalRenderJob> value;
};
#endif

namespace {
thread_local SonareVocalErrorDetail last_detail = [] {
  SonareVocalErrorDetail detail{};
  detail.struct_size = sizeof(detail);
  detail.schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  return detail;
}();

template <typename T>
void initialize(T* value) {
  if (!value) return;
  *value = {};
  value->struct_size = sizeof(T);
  value->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
}

template <typename T>
void require_pointer(const T* value, const char* field) {
  if (!value) throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, field);
}

template <typename T>
void require_header(const T* value, const char* field) {
  require_pointer(value, field);
  if (value->struct_size < sizeof(T) || value->schema_version != SONARE_VOCAL_EDIT_API_VERSION) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, field);
  }
}

template <typename T>
void prepare(T* value) {
  require_header(value, "result header");
  initialize(value);
}

template <typename T>
size_t checked_count(const T* data, uint64_t count, const char* field) {
  if (count > sonare_c_detail::kMaxBufferSize || count > SIZE_MAX / sizeof(T) || (count && !data)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, field);
  }
  return static_cast<size_t>(count);
}

template <typename T>
std::vector<T> copy_input(const T* data, uint64_t count, const char* field) {
  const size_t n = checked_count(data, count, field);
  return n ? std::vector<T>(data, data + n) : std::vector<T>{};
}

template <typename T>
std::unique_ptr<T[]> copy_output(const std::vector<T>& data) {
  if (data.empty()) return {};
  auto out = std::make_unique<T[]>(data.size());
  std::copy(data.begin(), data.end(), out.get());
  return out;
}

template <size_t N>
void copy_text(char (&out)[N], const std::string& text) {
  const size_t n = std::min(text.size(), N - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
}

template <typename F>
SonareError invoke(F&& action) {
  sonare_c_detail::clear_last_error();
  initialize(&last_detail);
  try {
    action();
    return SONARE_OK;
#if defined(SONARE_WITH_PITCH_EDITOR)
  } catch (const vocal::VocalEditException& error) {
    last_detail.reason = static_cast<uint32_t>(error.reason());
    copy_text(last_detail.field, error.field());
    copy_text(last_detail.expected_text, error.expected());
    copy_text(last_detail.actual_text, error.actual());
    std::from_chars(error.expected().data(), error.expected().data() + error.expected().size(),
                    last_detail.expected);
    std::from_chars(error.actual().data(), error.actual().data() + error.actual().size(),
                    last_detail.actual);
    sonare_c_detail::set_last_error(error.what());
    return sonare_c_detail::map_sonare_exception(error);
#endif
  } catch (const sonare::SonareException& error) {
    last_detail.reason = error.code() == sonare::ErrorCode::NotImplemented
                             ? SONARE_VOCAL_REASON_UNSUPPORTED
                             : SONARE_VOCAL_REASON_INVALID_INPUT;
    sonare_c_detail::set_last_error(error.what());
    return sonare_c_detail::map_sonare_exception(error);
  } catch (const std::bad_alloc&) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error("vocal edit allocation failed");
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (const std::exception& error) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error(error.what());
    return SONARE_ERROR_UNKNOWN;
  } catch (...) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error(sonare_c_detail::kUnknownExceptionMessage);
    return SONARE_ERROR_UNKNOWN;
  }
}
}  // namespace

uint32_t sonare_vocal_edit_api_version(void) { return SONARE_VOCAL_EDIT_API_VERSION; }

void sonare_vocal_analysis_init(SonareVocalAnalysis* analysis) {
  if (!analysis) return;
  *analysis = {};
  analysis->struct_size = sizeof(*analysis);
  analysis->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  const sonare::PitchConfig pitch;
  analysis->samples_per_frame = pitch.hop_length;
  analysis->frame_length_samples = static_cast<uint32_t>(pitch.frame_length);
  analysis->algorithm_id = "host";
  analysis->algorithm_version = 1;
  analysis->fmin_hz = pitch.fmin;
  analysis->fmax_hz = pitch.fmax;
  analysis->yin_threshold = pitch.threshold;
  analysis->voiced_threshold = 0.5;
  analysis->centered = pitch.center;
  analysis->segmentation_threshold_cents = 50.0;
  analysis->min_note_ms = 30.0;
  analysis->reference_hz = sonare::constants::kA4Hz;
}

void sonare_vocal_create_options_init(SonareVocalCreateOptions* options) {
  if (!options) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  options->edge_fade_ms = 5.0;
  options->vibrato_cutoff_hz = 3.0;
  options->segmentation_threshold_cents = 50.0;
  options->min_note_ms = 30.0;
  const sonare::PitchConfig pitch;
  options->frame_length_samples = static_cast<uint32_t>(pitch.frame_length);
  options->hop_length_samples = static_cast<uint32_t>(pitch.hop_length);
  options->fmin_hz = pitch.fmin;
  options->fmax_hz = pitch.fmax;
  options->yin_threshold = pitch.threshold;
  options->voiced_threshold = 0.5;
  options->centered = pitch.center;
  options->reference_hz = sonare::constants::kA4Hz;
  options->max_history_bytes = 64u * 1024u * 1024u;
  options->max_cache_bytes = 128u * 1024u * 1024u;
  options->max_undo_depth = 128;
  options->max_render_jobs = 4;
}

void sonare_vocal_restore_options_init(SonareVocalRestoreOptions* options) {
  if (!options) return;
  SonareVocalCreateOptions create;
  sonare_vocal_create_options_init(&create);
  *options = {};
  options->struct_size = sizeof(*options);
  options->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  options->max_history_bytes = create.max_history_bytes;
  options->max_cache_bytes = create.max_cache_bytes;
  options->max_undo_depth = create.max_undo_depth;
  options->max_render_jobs = create.max_render_jobs;
}

void sonare_vocal_note_edit_init(SonareVocalNoteEdit* edit) {
  if (!edit) return;
  *edit = {};
  edit->struct_size = sizeof(*edit);
  edit->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  edit->max_correction_semitones = 12.0;
  edit->drift_scale = 1.0;
  edit->vibrato_scale = 1.0;
}

void sonare_vocal_operation_init(SonareVocalOperation* operation) {
  if (!operation) return;
  *operation = {};
  operation->struct_size = sizeof(*operation);
  operation->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  operation->merge_policy = SONARE_VOCAL_MERGE_PRESERVE;
  sonare_vocal_note_edit_init(&operation->edit);
  operation->transition.struct_size = sizeof(operation->transition);
  operation->transition.schema_version = SONARE_VOCAL_EDIT_API_VERSION;
}

void sonare_vocal_notes_result_init(SonareVocalNotesResult* v) { initialize(v); }
void sonare_vocal_analysis_result_init(SonareVocalAnalysisResult* v) { initialize(v); }
void sonare_vocal_edit_result_init(SonareVocalEditResult* v) { initialize(v); }
void sonare_vocal_pitch_result_init(SonareVocalPitchResult* v) { initialize(v); }
void sonare_vocal_render_result_init(SonareVocalRenderResult* v) { initialize(v); }
void sonare_vocal_state_bytes_init(SonareVocalStateBytes* v) { initialize(v); }
void sonare_vocal_capabilities_init(SonareVocalCapabilities* v) { initialize(v); }
void sonare_vocal_error_detail_init(SonareVocalErrorDetail* v) { initialize(v); }
void sonare_vocal_last_error_detail(SonareVocalErrorDetail* v) {
  if (v && v->struct_size >= sizeof(*v) && v->schema_version == SONARE_VOCAL_EDIT_API_VERSION)
    *v = last_detail;
}

int sonare_vocal_available(void) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  return 1;
#else
  return 0;
#endif
}

void sonare_vocal_free_notes(SonareVocalNotesResult* v) {
  if (!v) return;
  for (uint64_t i = 0; i < v->note_count; ++i) {
    delete[] v->notes[i].amplitude;
    delete[] v->notes[i].edit.target_points;
    delete[] v->notes[i].edit.amplitude_envelope;
  }
  delete[] v->notes;
  delete[] v->transitions;
  initialize(v);
}
void sonare_vocal_free_analysis(SonareVocalAnalysisResult* v) {
  if (!v) return;
  delete[] v->analysis.f0_hz;
  delete[] v->analysis.voiced;
  delete[] v->analysis.algorithm_id;
  initialize(v);
}
void sonare_vocal_free_edit_result(SonareVocalEditResult* v) {
  if (!v) return;
  delete[] v->dirty_ranges;
  delete[] v->id_changes;
  initialize(v);
}
void sonare_vocal_free_pitch_result(SonareVocalPitchResult* v) {
  if (!v) return;
  delete[] v->source_samples;
  delete[] v->measured_midi;
  delete[] v->target_midi;
  delete[] v->effective_midi;
  delete[] v->voiced;
  delete[] v->has_target;
  initialize(v);
}
void sonare_vocal_free_render_result(SonareVocalRenderResult* v) {
  if (!v) return;
  delete[] v->samples;
  delete[] v->processed_ranges;
  initialize(v);
}
void sonare_vocal_free_state_bytes(SonareVocalStateBytes* v) {
  if (!v) return;
  delete[] v->data;
  initialize(v);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
#define VOCAL_SUPPORTED_BODY(...) __VA_ARGS__
#define VOCAL_ACTION(...) return invoke([&] { VOCAL_SUPPORTED_BODY(__VA_ARGS__); })
namespace {
SonareVocalStateToken map_token(const vocal::VocalStateToken& v) {
  return {v.epoch, v.committed_revision, v.draft_id, v.draft_generation, 0, 1};
}
std::string hex(const vocal::Sha256Digest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (auto byte : digest) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}
vocal::VocalNoteEdit import_edit(const SonareVocalNoteEdit& v) {
  require_header(&v, "edit header");
  if (v.muted > 1)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "muted must be boolean",
                                    "muted");
  vocal::VocalNoteEdit e;
  e.pitch.target.mode = static_cast<vocal::PitchTargetMode>(v.target_mode);
  e.pitch.target.center_midi = v.target_midi;
  checked_count(v.target_points, v.target_point_count, "target_points");
  for (uint64_t i = 0; i < v.target_point_count; ++i)
    e.pitch.target.points.push_back({v.target_points[i].source_sample, v.target_points[i].midi});
  e.pitch.amount = v.amount;
  e.pitch.speed_ms = v.speed_ms;
  e.pitch.max_correction_semitones = v.max_correction_semitones;
  e.pitch.transpose_semitones = v.transpose_semitones;
  e.pitch.drift_scale = v.drift_scale;
  e.pitch.vibrato_scale = v.vibrato_scale;
  e.destination_start_sample = v.destination_start_sample;
  e.destination_length_samples = v.destination_length_samples;
  e.gain_db = v.gain_db;
  e.muted = v.muted != 0;
  e.formant.mode = static_cast<vocal::FormantMode>(v.formant_mode);
  e.formant.shift_semitones = v.formant_shift_semitones;
  e.amplitude_envelope =
      copy_input(v.amplitude_envelope, v.amplitude_envelope_count, "amplitude_envelope");
  return e;
}
vocal::PitchTransition import_transition(const SonareVocalTransition& v) {
  require_header(&v, "transition header");
  vocal::PitchTransition t;
  t.left_note_id = v.left_note_id;
  t.right_note_id = v.right_note_id;
  t.left_window_samples = v.left_window_samples;
  t.right_window_samples = v.right_window_samples;
  t.strength = v.strength;
  return t;
}
vocal::Operation import_operation(const SonareVocalOperation& v) {
  require_header(&v, "operation header");
  switch (v.kind) {
    case SONARE_VOCAL_SET_EDIT:
      return vocal::SetNoteEditOp{v.note_id, import_edit(v.edit)};
    case SONARE_VOCAL_SET_SOURCE_SPAN:
      return vocal::SetNoteSourceSpanOp{v.note_id,
                                        {v.source_start_sample, v.source_end_sample},
                                        v.destination_start_sample,
                                        v.destination_length_samples};
    case SONARE_VOCAL_SPLIT:
      return vocal::SplitNoteOp{v.note_id, v.cut_source_sample};
    case SONARE_VOCAL_MERGE:
      return vocal::MergeNotesOp{copy_input(v.note_ids, v.note_id_count, "note_ids"),
                                 static_cast<vocal::MergeMode>(v.merge_policy)};
    case SONARE_VOCAL_SET_TRANSITION:
      return vocal::SetTransitionOp{import_transition(v.transition)};
    case SONARE_VOCAL_REMOVE_TRANSITION:
      return vocal::RemoveTransitionOp{v.transition.left_note_id, v.transition.right_note_id};
    case SONARE_VOCAL_RESET:
      return vocal::ResetNotesOp{copy_input(v.note_ids, v.note_id_count, "note_ids")};
    default:
      throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "unknown operation",
                                      "kind");
  }
}
SonareVocalTransition map_transition(const vocal::PitchTransition& t) {
  SonareVocalTransition v;
  initialize(&v);
  v.left_note_id = t.left_note_id;
  v.right_note_id = t.right_note_id;
  v.left_window_samples = t.left_window_samples;
  v.right_window_samples = t.right_window_samples;
  v.strength = t.strength;
  return v;
}
void map_edit(const vocal::VocalNoteEdit& e, SonareVocalNoteEdit& v) {
  sonare_vocal_note_edit_init(&v);
  v.target_mode = static_cast<uint32_t>(e.pitch.target.mode);
  v.target_midi = e.pitch.target.center_midi;
  auto points = std::make_unique<SonareVocalPitchPoint[]>(e.pitch.target.points.size());
  for (size_t i = 0; i < e.pitch.target.points.size(); ++i)
    points[i] = {e.pitch.target.points[i].source_sample, e.pitch.target.points[i].target_midi};
  auto envelope = copy_output(e.amplitude_envelope);
  v.amount = e.pitch.amount;
  v.speed_ms = e.pitch.speed_ms;
  v.max_correction_semitones = e.pitch.max_correction_semitones;
  v.transpose_semitones = e.pitch.transpose_semitones;
  v.drift_scale = e.pitch.drift_scale;
  v.vibrato_scale = e.pitch.vibrato_scale;
  v.destination_start_sample = e.destination_start_sample;
  v.destination_length_samples = e.destination_length_samples;
  v.gain_db = e.gain_db;
  v.muted = e.muted;
  v.formant_mode = static_cast<uint32_t>(e.formant.mode);
  v.formant_shift_semitones = e.formant.shift_semitones;
  v.target_points = points.release();
  v.target_point_count = e.pitch.target.points.size();
  v.amplitude_envelope = envelope.release();
  v.amplitude_envelope_count = e.amplitude_envelope.size();
}
template <typename Owner>
void map_notes(const Owner& owner, SonareVocalNotesResult* out) {
  const auto notes = owner.notes();
  const auto transitions = owner.transitions();
  const auto& analysis = owner.analysis();
  SonareVocalNotesResult local;
  initialize(&local);
  struct Cleanup {
    SonareVocalNotesResult* p;
    ~Cleanup() { sonare_vocal_free_notes(p); }
  } cleanup{&local};
  local.notes = new SonareVocalNote[notes.size()]{};
  local.note_count = notes.size();
  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& n = notes[i];
    auto& v = local.notes[i];
    initialize(&v);
    v.id = n.id;
    v.source_start_sample = n.source_range.start;
    v.source_end_sample = n.source_range.end;
    v.analysis_frame_start = n.analysis_frame_start;
    v.analysis_frame_end = n.analysis_frame_end;
    v.has_pitch = n.has_pitch;
    v.center_midi = n.centre_midi;
    v.median_hz = n.median_hz;
    v.f0_stability = n.f0_stability;
    map_edit(n.edit, v.edit);
    const auto first = analysis.amplitude.begin() + n.analysis_frame_start;
    std::vector<float> amplitude(first, analysis.amplitude.begin() + n.analysis_frame_end);
    v.amplitude = copy_output(amplitude).release();
    v.amplitude_count = amplitude.size();
  }
  local.transitions = new SonareVocalTransition[transitions.size()]{};
  local.transition_count = transitions.size();
  for (size_t i = 0; i < local.transition_count; ++i)
    local.transitions[i] = map_transition(transitions[i]);
  *out = local;
  initialize(&local);
}
void map_ranges(const std::vector<vocal::SampleRange>& ranges, SonareVocalRange*& data,
                uint64_t& count) {
  auto out = std::make_unique<SonareVocalRange[]>(ranges.size());
  for (size_t i = 0; i < ranges.size(); ++i) out[i] = {ranges[i].start, ranges[i].end};
  data = out.release();
  count = ranges.size();
}
template <typename Change>
void map_change(const Change& change, SonareVocalEditResult* out) {
  out->token = map_token(change.token);
  map_ranges(change.dirty_ranges, out->dirty_ranges, out->dirty_range_count);
}
void map_apply(const vocal::DraftApplyResult& change, SonareVocalEditResult* out) {
  auto ids = std::make_unique<SonareVocalIdChange[]>(change.id_changes.size());
  for (size_t i = 0; i < change.id_changes.size(); ++i) {
    const auto& id = change.id_changes[i];
    ids[i] = {id.operation_index, id.retired_id, id.new_ids.empty() ? 0 : id.new_ids[0],
              id.new_ids.size() < 2 ? 0 : id.new_ids[1]};
  }
  map_change(change, out);
  out->id_changes = ids.release();
  out->id_change_count = change.id_changes.size();
}
struct PreparedEditResult {
  SonareVocalEditResult value;
  PreparedEditResult() { initialize(&value); }
  ~PreparedEditResult() { sonare_vocal_free_edit_result(&value); }
  void publish(SonareVocalEditResult* out) noexcept {
    *out = value;
    initialize(&value);
  }
};
template <typename Plans>
void map_pitch(const Plans& plans, SonareVocalPitchResult* out) {
  size_t count = 0;
  for (const auto& plan : plans) count += plan.points.size();
  auto source = std::make_unique<double[]>(count), measured = std::make_unique<double[]>(count);
  auto target = std::make_unique<double[]>(count), effective = std::make_unique<double[]>(count);
  auto voiced = std::make_unique<uint8_t[]>(count), has_target = std::make_unique<uint8_t[]>(count);
  size_t i = 0;
  for (const auto& plan : plans)
    for (const auto& point : plan.points) {
      source[i] = point.source_sample;
      measured[i] = point.measured_midi;
      target[i] = point.target_midi;
      effective[i] = point.effective_midi;
      voiced[i] = point.voiced;
      has_target[i] = point.has_target;
      ++i;
    }
  out->source_samples = source.release();
  out->measured_midi = measured.release();
  out->target_midi = target.release();
  out->effective_midi = effective.release();
  out->voiced = voiced.release();
  out->has_target = has_target.release();
  out->frame_count = count;
}
void map_render(vocal::VocalRenderResult rendered, SonareVocalRenderResult* out) {
  auto samples = copy_output(rendered.samples);
  map_ranges(rendered.processed_ranges, out->processed_ranges, out->processed_range_count);
  out->samples = samples.release();
  out->sample_count = static_cast<int64_t>(rendered.samples.size());
  out->start_sample = rendered.start_sample;
  out->token = map_token(rendered.state_token);
  out->token.request_id = rendered.request_id;
  out->token.profile_id = static_cast<uint32_t>(rendered.profile);
  out->cache_hit_units = rendered.cache_hit_units;
  out->dry_passed_frames = rendered.diagnostics.dry_passed_frames;
  out->limited_correction_frames = rendered.diagnostics.limited_correction_frames;
}
vocal::VocalCancelProbe cancel_probe(SonareVocalCancelCallback callback, void* data) {
  return callback ? vocal::VocalCancelProbe([=] { return callback(data) != 0; })
                  : vocal::VocalCancelProbe{};
}
sonare::Audio import_source(const float* samples, int64_t frames, int channels, int rate) {
  if (channels != 1 || frames <= 0 ||
      static_cast<uint64_t>(frames) > sonare_c_detail::kMaxBufferSize)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "positive mono source required", "source");
  if (rate < sonare::kMinAudioSampleRate || rate > sonare::kMaxAudioSampleRate)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "sample rate is outside the supported range", "sample_rate",
                                    std::to_string(sonare::kMinAudioSampleRate) + ".." +
                                        std::to_string(sonare::kMaxAudioSampleRate),
                                    std::to_string(rate));
  require_pointer(samples, "samples");
  return sonare::Audio::from_buffer(samples, static_cast<size_t>(frames), rate);
}
vocal::VocalSessionCreateOptions import_options(const SonareVocalCreateOptions* input) {
  SonareVocalCreateOptions defaults;
  sonare_vocal_create_options_init(&defaults);
  const auto& v = input ? *input : defaults;
  require_header(&v, "create options header");
  vocal::VocalSessionCreateOptions options;
  options.output_length_samples = v.output_length_samples;
  options.render_settings.edge_fade_ms = v.edge_fade_ms;
  options.render_settings.vibrato_cutoff_hz = v.vibrato_cutoff_hz;
  options.analysis_options.segmentation_threshold_cents = v.segmentation_threshold_cents;
  options.analysis_options.minimum_note_ms = v.min_note_ms;
  if (v.frame_length_samples > INT_MAX || v.hop_length_samples > INT_MAX)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "analysis dimensions too large", "analysis");
  options.analysis_options.pitch.frame_length = static_cast<int>(v.frame_length_samples);
  options.analysis_options.pitch.hop_length = static_cast<int>(v.hop_length_samples);
  options.analysis_options.pitch.fmin = static_cast<float>(v.fmin_hz);
  options.analysis_options.pitch.fmax = static_cast<float>(v.fmax_hz);
  if (v.centered > 1)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "invalid centered flag",
                                    "centered");
  options.analysis_options.pitch.threshold = static_cast<float>(v.yin_threshold);
  options.analysis_options.pitch.center = v.centered != 0;
  options.analysis_options.voiced_threshold = v.voiced_threshold;
  options.analysis_options.reference_hz = v.reference_hz;
  options.limits = {v.max_history_bytes, v.max_cache_bytes, v.max_undo_depth, v.max_render_jobs};
  if (v.analysis) {
    const auto& a = *v.analysis;
    require_header(&a, "analysis header");
    vocal::VocalAnalysisData analysis;
    analysis.grid = {a.frame_origin_sample, a.samples_per_frame, a.frame_length_samples};
    analysis.f0_hz = copy_input(a.f0_hz, a.frame_count, "f0_hz");
    analysis.voiced = copy_input(a.voiced, a.frame_count, "voiced");
    require_pointer(a.algorithm_id, "algorithm_id");
    size_t length = 0;
    while (length < 256 && a.algorithm_id[length]) ++length;
    if (length == 256)
      throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "algorithm id too long",
                                      "algorithm_id");
    analysis.algorithm_id.assign(a.algorithm_id, length);
    analysis.algorithm_version = a.algorithm_version;
    if (a.centered > 1)
      throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "invalid centered flag",
                                      "analysis.centered");
    analysis.settings.fmin_hz = a.fmin_hz;
    analysis.settings.fmax_hz = a.fmax_hz;
    analysis.settings.yin_threshold = a.yin_threshold;
    analysis.settings.voiced_threshold = a.voiced_threshold;
    analysis.settings.centered = a.centered != 0;
    analysis.settings.segmentation_threshold_cents = a.segmentation_threshold_cents;
    analysis.settings.minimum_note_ms = a.min_note_ms;
    analysis.settings.reference_hz = a.reference_hz;
    options.analysis = std::move(analysis);
  }
  return options;
}
}  // namespace
#else
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define VOCAL_SUPPORTED_BODY(...) \
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented, "vocal editing unavailable")
#define VOCAL_ACTION(...) return invoke([] { VOCAL_SUPPORTED_BODY(__VA_ARGS__); })
#endif
#define VOCAL_OUTPUT_ACTION(out, ...)  \
  return invoke([&] {                  \
    prepare(out);                      \
    VOCAL_SUPPORTED_BODY(__VA_ARGS__); \
  })

SonareError sonare_vocal_session_create(const float* samples, int64_t frames, int channels,
                                        int sample_rate, const SonareVocalCreateOptions* options,
                                        SonareVocalEditSession** out_session) {
  if (out_session) *out_session = nullptr;
  VOCAL_ACTION(require_pointer(out_session, "out_session");
               auto session = vocal::VocalEditSession::create(
                   import_source(samples, frames, channels, sample_rate), import_options(options));
               *out_session = new SonareVocalEditSession{std::move(session)});
}
SonareError sonare_vocal_session_restore(const float* samples, int64_t frames, int channels,
                                         int sample_rate, const uint8_t* state, uint64_t size,
                                         const SonareVocalRestoreOptions* input,
                                         SonareVocalEditSession** out_session) {
  if (out_session) *out_session = nullptr;
  SonareVocalRestoreOptions defaults;
  sonare_vocal_restore_options_init(&defaults);
  const auto& v = input ? *input : defaults;
  VOCAL_ACTION(
      require_pointer(out_session, "out_session"); require_header(&v, "restore options header");
      auto bytes = copy_input(state, size, "state"); const vocal::VocalSessionLimits limits{
          v.max_history_bytes, v.max_cache_bytes, v.max_undo_depth, v.max_render_jobs};
      auto session = vocal::VocalEditSession::restore(
          import_source(samples, frames, channels, sample_rate), bytes, limits);
      *out_session = new SonareVocalEditSession{std::move(session)});
}
SonareError sonare_vocal_session_notes(const SonareVocalEditSession* s,
                                       SonareVocalNotesResult* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "session"); map_notes(s->value, out));
}
SonareError sonare_vocal_draft_notes(const SonareVocalEditDraft* s, SonareVocalNotesResult* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "draft"); map_notes(*s->value, out));
}
SonareError sonare_vocal_session_token(const SonareVocalEditSession* s,
                                       SonareVocalStateToken* out) {
  if (out) *out = {};
  VOCAL_ACTION(require_pointer(out, "token"); require_pointer(s, "session");
               *out = map_token(s->value.token()));
}
SonareError sonare_vocal_draft_token(const SonareVocalEditDraft* s, SonareVocalStateToken* out) {
  if (out) *out = {};
  VOCAL_ACTION(require_pointer(out, "token"); require_pointer(s, "draft");
               *out = map_token(s->value->token()));
}
SonareError sonare_vocal_session_revision(const SonareVocalEditSession* s, uint64_t* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "revision"); require_pointer(s, "session");
               *out = s->value.token().committed_revision);
}
SonareError sonare_vocal_session_output_length(const SonareVocalEditSession* s, int64_t* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "output length"); require_pointer(s, "session");
               *out = s->value.output_length_samples());
}
SonareError sonare_vocal_session_history(const SonareVocalEditSession* s, int* can_undo,
                                         int* can_redo) {
  if (can_undo) *can_undo = 0;
  if (can_redo) *can_redo = 0;
  VOCAL_ACTION(require_pointer(can_undo, "can_undo"); require_pointer(can_redo, "can_redo");
               require_pointer(s, "session"); *can_undo = s->value.can_undo();
               *can_redo = s->value.can_redo());
}
SonareError sonare_vocal_snapshot_output_length(const SonareVocalRenderSnapshot* s, int64_t* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "output length"); require_pointer(s, "snapshot");
               *out = s->value->data().output_length_samples);
}
SonareError sonare_vocal_session_analysis(const SonareVocalEditSession* s,
                                          SonareVocalAnalysisResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "session"); auto a = s->value.analysis();
      auto descriptor = s->value.source_descriptor(); auto f0 = copy_output(a.f0_hz);
      auto voiced = copy_output(a.voiced);
      auto name = std::make_unique<char[]>(a.algorithm_id.size() + 1);
      std::memcpy(name.get(), a.algorithm_id.c_str(), a.algorithm_id.size() + 1);
      SonareVocalAnalysisResult local; initialize(&local);
      sonare_vocal_analysis_init(&local.analysis);
      local.analysis.frame_origin_sample = a.grid.frame_origin_sample;
      local.analysis.samples_per_frame = a.grid.samples_per_frame;
      local.analysis.frame_length_samples = a.grid.frame_length_samples;
      local.analysis.algorithm_version = a.algorithm_version;
      local.analysis.fmin_hz = a.settings.fmin_hz; local.analysis.fmax_hz = a.settings.fmax_hz;
      local.analysis.yin_threshold = a.settings.yin_threshold;
      local.analysis.voiced_threshold = a.settings.voiced_threshold;
      local.analysis.centered = a.settings.centered;
      local.analysis.segmentation_threshold_cents = a.settings.segmentation_threshold_cents;
      local.analysis.min_note_ms = a.settings.minimum_note_ms;
      local.analysis.reference_hz = a.settings.reference_hz;

      local.analysis.frame_count = a.f0_hz.size();
      copy_text(local.source_sha256, hex(descriptor.digest));
      copy_text(local.analysis_sha256, hex(a.digest));
      local.source_length_samples = descriptor.sample_count;
      local.sample_rate = descriptor.sample_rate; local.analysis.f0_hz = f0.release();
      local.analysis.voiced = voiced.release(); local.analysis.algorithm_id = name.release();
      *out = local);
}
SonareError sonare_vocal_session_capabilities(const SonareVocalEditSession* s,
                                              SonareVocalCapabilities* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "session"); const auto c = s->value.capabilities();
      out->api_version = c.api_version; out->profile_id = static_cast<uint32_t>(c.profile);
      out->monophonic_only = c.monophonic_only; out->analysis_cancellable = c.analysis_cancellable;
      out->minimum_formant_shift_semitones = c.min_formant_shift_semitones;
      out->maximum_formant_shift_semitones = c.max_formant_shift_semitones);
}
SonareError sonare_vocal_session_begin_edit(SonareVocalEditSession* s, uint64_t revision,
                                            SonareVocalEditDraft** out) {
  if (out) *out = nullptr;
  VOCAL_ACTION(require_pointer(out, "draft output"); require_pointer(s, "session");
               auto holder = std::make_unique<SonareVocalEditDraft>();
               holder->value = s->value.begin_edit(revision); *out = holder.release());
}
SonareError sonare_vocal_draft_apply(SonareVocalEditDraft* s, uint64_t generation,
                                     const SonareVocalOperation* operations, uint64_t count,
                                     SonareVocalEditResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "draft"); checked_count(operations, count, "operations");
      std::vector<vocal::Operation> ops; ops.reserve(static_cast<size_t>(count));
      for (uint64_t i = 0; i < count; ++i) ops.push_back(import_operation(operations[i]));
      PreparedEditResult prepared; s->value->apply(
          generation, ops, [&](const auto& change) { map_apply(change, &prepared.value); });
      prepared.publish(out));
}
SonareError sonare_vocal_draft_commit(SonareVocalEditDraft* s, uint64_t revision,
                                      SonareVocalEditResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "draft"); PreparedEditResult prepared;
      s->value->commit(revision, [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(out));
}
SonareError sonare_vocal_draft_cancel(SonareVocalEditDraft* s) {
  VOCAL_ACTION(require_pointer(s, "draft"); s->value->cancel());
}
SonareError sonare_vocal_session_undo(SonareVocalEditSession* s, uint64_t revision,
                                      SonareVocalEditResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "session"); PreparedEditResult prepared;
      s->value.undo(revision, [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(out));
}
SonareError sonare_vocal_session_redo(SonareVocalEditSession* s, uint64_t revision,
                                      SonareVocalEditResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "session"); PreparedEditResult prepared;
      s->value.redo(revision, [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(out));
}
SonareError sonare_vocal_session_evaluate_pitch(const SonareVocalEditSession* s, uint32_t id,
                                                SonareVocalPitchResult* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "session");
                      map_pitch(s->value.evaluate_pitch({id}), out));
}
SonareError sonare_vocal_draft_evaluate_pitch(const SonareVocalEditDraft* s, uint32_t id,
                                              SonareVocalPitchResult* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "draft");
                      map_pitch(s->value->evaluate_pitch({id}), out));
}
SonareError sonare_vocal_session_source_to_destination(const SonareVocalEditSession* s, uint32_t id,
                                                       double value, double* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "coordinate output"); require_pointer(s, "session");
               *out = s->value.source_sample_to_destination_sample(id, value));
}
SonareError sonare_vocal_session_destination_to_source(const SonareVocalEditSession* s, uint32_t id,
                                                       double value, double* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "coordinate output"); require_pointer(s, "session");
               *out = s->value.destination_sample_to_source_sample(id, value));
}
SonareError sonare_vocal_draft_source_to_destination(const SonareVocalEditDraft* s, uint32_t id,
                                                     double value, double* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "coordinate output"); require_pointer(s, "draft");
               *out = s->value->source_sample_to_destination_sample(id, value));
}
SonareError sonare_vocal_draft_destination_to_source(const SonareVocalEditDraft* s, uint32_t id,
                                                     double value, double* out) {
  if (out) *out = 0;
  VOCAL_ACTION(require_pointer(out, "coordinate output"); require_pointer(s, "draft");
               *out = s->value->destination_sample_to_source_sample(id, value));
}
SonareError sonare_vocal_session_capture_snapshot(const SonareVocalEditSession* s,
                                                  SonareVocalRenderSnapshot** out) {
  if (out) *out = nullptr;
  VOCAL_ACTION(require_pointer(out, "snapshot output"); require_pointer(s, "session");
               *out = new SonareVocalRenderSnapshot{s->value.capture_render_snapshot()});
}
SonareError sonare_vocal_draft_capture_snapshot(const SonareVocalEditDraft* s,
                                                SonareVocalRenderSnapshot** out) {
  if (out) *out = nullptr;
  VOCAL_ACTION(require_pointer(out, "snapshot output"); require_pointer(s, "draft");
               *out = new SonareVocalRenderSnapshot{s->value->capture_render_snapshot()});
}
SonareError sonare_vocal_snapshot_render(const SonareVocalRenderSnapshot* s, SonareVocalRange range,
                                         uint64_t request, SonareVocalCancelCallback cancel,
                                         void* data, SonareVocalRenderResult* out) {
  VOCAL_OUTPUT_ACTION(
      out, require_pointer(s, "snapshot");
      map_render(vocal::render_snapshot(s->value, {{range.start_sample, range.end_sample}, request},
                                        cancel_probe(cancel, data)),
                 out));
}
SonareError sonare_vocal_render_job_begin(const SonareVocalRenderSnapshot* s,
                                          SonareVocalRange range, uint64_t request,
                                          SonareVocalRenderJob** out) {
  if (out) *out = nullptr;
  VOCAL_ACTION(
      require_pointer(out, "job output"); require_pointer(s, "snapshot");
      auto holder = std::make_unique<SonareVocalRenderJob>();
      holder->value = std::make_unique<vocal::VocalRenderJob>(
          s->value, vocal::VocalRenderRequest{{range.start_sample, range.end_sample}, request});
      *out = holder.release());
}
SonareError sonare_vocal_render_job_next(SonareVocalRenderJob* s, SonareVocalCancelCallback cancel,
                                         void* data, int* complete) {
  if (complete) *complete = 0;
  VOCAL_ACTION(require_pointer(complete, "complete"); require_pointer(s, "job");
               s->value->next(cancel_probe(cancel, data));
               const auto state = s->value->progress().state;
               if (state == vocal::VocalRenderJobState::kAborted) throw vocal::VocalEditException(
                   vocal::VocalReason::kCancelled, "render job aborted", "render");
               *complete = state == vocal::VocalRenderJobState::kComplete ? 1 : 0);
}
SonareError sonare_vocal_render_job_finalize(SonareVocalRenderJob* s,
                                             SonareVocalCancelCallback cancel, void* data,
                                             SonareVocalRenderResult* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "job");
                      map_render(s->value->finalize(cancel_probe(cancel, data)), out));
}
SonareError sonare_vocal_session_export_state(const SonareVocalEditSession* s,
                                              SonareVocalStateBytes* out) {
  VOCAL_OUTPUT_ACTION(out, require_pointer(s, "session"); auto bytes = s->value.export_state();
                      auto data = copy_output(bytes); out->size = bytes.size();
                      out->data = data.release());
}

void sonare_vocal_session_destroy(SonareVocalEditSession* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
void sonare_vocal_draft_destroy(SonareVocalEditDraft* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
void sonare_vocal_snapshot_destroy(SonareVocalRenderSnapshot* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
void sonare_vocal_render_job_abort(SonareVocalRenderJob* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (s) s->value->abort();
#else
  (void)s;
#endif
}
void sonare_vocal_render_job_destroy(SonareVocalRenderJob* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
#undef VOCAL_ACTION
#undef VOCAL_OUTPUT_ACTION
#undef VOCAL_SUPPORTED_BODY
