#include <sonare/sonare_c_vocal_edit.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "editing_input_domains.h"
#include "feature/pitch.h"
#include "sonare_c_internal.h"
#include "util/constants.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "editing/vocal_edit/state_codec.h"
namespace vocal = sonare::editing::vocal_edit;
struct SonareVocalSession {
  vocal::VocalEditSession value;
};
struct SonareVocalDraft {
  std::unique_ptr<vocal::VocalEditDraft> value;
};
struct SonareVocalSnapshot {
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
  return detail;
}();

template <typename T>
void initialize(T* value) {
  if (!value) return;
  *value = {};
  value->struct_size = sizeof(T);
}

template <typename T>
void require_pointer(const T* value, const char* field) {
  if (!value) throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, field);
}

template <typename T>
void require_header(const T* value, const char* path) {
  require_pointer(value, path);
  if (value->struct_size < sizeof(T)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, path);
  }
}

/// Whether a result struct carries the header its init function writes.
template <typename T>
bool result_header_valid(const T* value) {
  return value && value->struct_size >= sizeof(T);
}

/// Refuses a result struct whose header was not valid when the call began.
/// @details Unread when the pitch editor is compiled out, whose bodies discard their arguments.
[[maybe_unused]] void require_result_header(bool valid, const void* value, const char* path) {
  if (valid) return;
  if (!value) throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, path);
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                std::string(path) + ".struct_size");
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
void copy_text(char (&out)[N], std::string_view text) noexcept {
  const size_t n = std::min(text.size(), N - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
}

template <typename F>
SonareError invoke(F&& action) {
  sonare_c_detail::clear_last_error();
  initialize(&last_detail);
  const char* expected_subject = nullptr;
  try {
    action(expected_subject);
    return SONARE_OK;
#if defined(SONARE_WITH_PITCH_EDITOR)
  } catch (const vocal::VocalEditException& error) {
    last_detail.reason = static_cast<uint32_t>(error.reason());
    const std::string_view field =
        error.reason() == vocal::VocalReason::kRevisionConflict && expected_subject
            ? std::string_view(expected_subject)
            : std::string_view(error.field());
    copy_text(last_detail.field, field);
    copy_text(last_detail.expected_text, error.expected());
    copy_text(last_detail.actual_text, error.actual());
    std::from_chars(error.expected().data(), error.expected().data() + error.expected().size(),
                    last_detail.expected);
    std::from_chars(error.actual().data(), error.actual().data() + error.actual().size(),
                    last_detail.actual);
    const SonareError code = sonare_c_detail::map_sonare_exception(error);
    sonare_c_detail::set_last_error(code, error.what());
    return code;
#endif
  } catch (const sonare::SonareParameterRefusal& refusal) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_INPUT;
    copy_text(last_detail.field, refusal.subject());
    return sonare_c_detail::record_current_exception();
  } catch (const sonare::SonareException& error) {
    last_detail.reason = error.code() == sonare::ErrorCode::NotSupported
                             ? SONARE_VOCAL_REASON_UNSUPPORTED
                             : SONARE_VOCAL_REASON_INVALID_INPUT;
    const SonareError code = sonare_c_detail::map_sonare_exception(error);
    sonare_c_detail::set_last_error(code, error.what());
    return code;
  } catch (const std::bad_alloc&) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error(SONARE_ERROR_OUT_OF_MEMORY, "vocal edit allocation failed");
    return SONARE_ERROR_OUT_OF_MEMORY;
  } catch (const std::exception& error) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error(SONARE_ERROR_UNKNOWN, error.what());
    return SONARE_ERROR_UNKNOWN;
  } catch (...) {
    last_detail.reason = SONARE_VOCAL_REASON_INVALID_STATE;
    sonare_c_detail::set_last_error(SONARE_ERROR_UNKNOWN,
                                    sonare_c_detail::kUnknownExceptionMessage);
    return SONARE_ERROR_UNKNOWN;
  }
}
}  // namespace

uint32_t sonare_vocal_edit_abi_version(void) { return SONARE_VOCAL_EDIT_ABI_VERSION; }

void sonare_vocal_analysis_init(SonareVocalAnalysis* analysis) {
  if (!analysis) return;
  *analysis = {};
  analysis->struct_size = sizeof(*analysis);
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
  options->max_history_bytes = create.max_history_bytes;
  options->max_cache_bytes = create.max_cache_bytes;
  options->max_undo_depth = create.max_undo_depth;
  options->max_render_jobs = create.max_render_jobs;
}

void sonare_vocal_note_edit_init(SonareVocalNoteEdit* edit) {
  if (!edit) return;
  *edit = {};
  edit->struct_size = sizeof(*edit);
  edit->max_correction_semitones = 12.0;
  edit->drift_scale = 1.0;
  edit->vibrato_scale = 1.0;
}

void sonare_vocal_operation_init(SonareVocalOperation* operation) {
  if (!operation) return;
  *operation = {};
  operation->struct_size = sizeof(*operation);
  operation->policy = SONARE_VOCAL_MERGE_PRESERVE;
  sonare_vocal_note_edit_init(&operation->edit);
  operation->transition.struct_size = sizeof(operation->transition);
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
  if (v && v->struct_size >= sizeof(*v)) *v = last_detail;
}

bool sonare_c_detail::vocal_edit_compiled_in() noexcept {
#if defined(SONARE_WITH_PITCH_EDITOR)
  return true;
#else
  return false;
#endif
}

void sonare_free_vocal_notes_result(SonareVocalNotesResult* v) {
  if (!v) return;
  for (size_t i = 0; i < v->note_count; ++i) {
    delete[] v->notes[i].amplitude;
    delete[] v->notes[i].edit.target_points;
    delete[] v->notes[i].edit.amplitude_envelope;
  }
  delete[] v->notes;
  delete[] v->transitions;
  initialize(v);
}
void sonare_free_vocal_analysis_result(SonareVocalAnalysisResult* v) {
  if (!v) return;
  delete[] v->analysis.f0_hz;
  delete[] v->analysis.voiced;
  delete[] v->analysis.algorithm_id;
  initialize(v);
}
void sonare_free_vocal_edit_result(SonareVocalEditResult* v) {
  if (!v) return;
  delete[] v->dirty_ranges;
  delete[] v->id_changes;
  initialize(v);
}
void sonare_free_vocal_pitch_result(SonareVocalPitchResult* v) {
  if (!v) return;
  delete[] v->source_samples;
  delete[] v->measured_midi;
  delete[] v->target_midi;
  delete[] v->effective_midi;
  delete[] v->voiced;
  delete[] v->has_target;
  initialize(v);
}
void sonare_free_vocal_render_result(SonareVocalRenderResult* v) {
  if (!v) return;
  delete[] v->samples;
  delete[] v->processed_ranges;
  initialize(v);
}
void sonare_free_vocal_state_bytes(SonareVocalStateBytes* v) {
  if (!v) return;
  delete[] v->data;
  initialize(v);
}

#if defined(SONARE_WITH_PITCH_EDITOR)
namespace {
static_assert(static_cast<int>(vocal::PitchTargetMode::kCurve) == SONARE_VOCAL_TARGET_CURVE &&
                  static_cast<int>(vocal::FormantMode::kShift) == SONARE_VOCAL_FORMANT_SHIFT &&
                  static_cast<int>(vocal::MergeMode::kResetEdits) == SONARE_VOCAL_MERGE_RESET,
              "SonareVocal* ordinals must mirror the vocal edit enums");
static_assert(vocal::VocalCapabilities{}.abi_version == SONARE_VOCAL_EDIT_ABI_VERSION,
              "VocalCapabilities::abi_version must equal SONARE_VOCAL_EDIT_ABI_VERSION");
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
  if (!sonare_c_detail::ordinal_to_enum(v.target_mode, SONARE_VOCAL_TARGET_NONE,
                                        SONARE_VOCAL_TARGET_CURVE, &e.pitch.target.mode)) {
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "unknown pitch target mode",
                                    "target_mode");
  }
  e.pitch.target.center_midi = v.target_midi;
  checked_count(v.target_points, v.target_point_count, "target_points");
  for (size_t i = 0; i < v.target_point_count; ++i)
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
  if (!sonare_c_detail::ordinal_to_enum(v.formant_mode, SONARE_VOCAL_FORMANT_PRESERVE,
                                        SONARE_VOCAL_FORMANT_SHIFT, &e.formant.mode)) {
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "unknown formant mode",
                                    "formant_mode");
  }
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
      return vocal::SplitNoteOp{v.note_id, v.source_sample};
    case SONARE_VOCAL_MERGE: {
      vocal::MergeMode merge = vocal::MergeMode::kPreserveEdits;
      if (!sonare_c_detail::ordinal_to_enum(v.policy, SONARE_VOCAL_MERGE_PRESERVE,
                                            SONARE_VOCAL_MERGE_RESET, &merge)) {
        throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "unknown merge policy",
                                        "policy");
      }
      return vocal::MergeNotesOp{copy_input(v.note_ids, v.note_id_count, "note_ids"), merge};
    }
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
    ~Cleanup() { sonare_free_vocal_notes_result(p); }
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
                size_t& count) {
  if (ranges.empty()) {
    data = nullptr;
    count = 0;
    return;
  }
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
  auto ids = change.id_changes.empty()
                 ? nullptr
                 : std::make_unique<SonareVocalIdChange[]>(change.id_changes.size());
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
  ~PreparedEditResult() { sonare_free_vocal_edit_result(&value); }
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
  out->sample_count = rendered.samples.size();
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
sonare::Audio import_source(const float* samples, int64_t frames, int channels, int sample_rate) {
  require_pointer(samples, "samples");
  if (frames <= 0 || static_cast<uint64_t>(frames) > sonare_c_detail::kMaxBufferSize)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "positive mono source required", "frames");
  if (channels != 1)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "positive mono source required", "channels");
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "sample rate is outside the supported range", "sample_rate",
                                    std::to_string(sonare::kMinAudioSampleRate) + ".." +
                                        std::to_string(sonare::kMaxAudioSampleRate),
                                    std::to_string(sample_rate));
  return sonare::Audio::from_buffer(samples, static_cast<size_t>(frames), sample_rate);
}
vocal::VocalSessionCreateOptions import_options(const SonareVocalCreateOptions* options) {
  SonareVocalCreateOptions defaults;
  sonare_vocal_create_options_init(&defaults);
  const auto& v = options ? *options : defaults;
  require_header(&v, "options.struct_size");
  // The options struct is a per-call parameter bag: every member is refused by its C path.
  namespace check = sonare::util::check;
  check::at_least(v.edge_fade_ms, 0.0, "options.edge_fade_ms");
  check::greater_than(v.vibrato_cutoff_hz, 0.0, "options.vibrato_cutoff_hz");
  if (!v.analysis) {
    check::greater_than(v.segmentation_threshold_cents, 0.0,
                        "options.segmentation_threshold_cents");
    check::greater_than(v.min_note_ms, 0.0, "options.min_note_ms");
    check::integer_within(v.frame_length_samples, sonare_c_detail::kVocalCreateFrameLengthDomain,
                          "options.frame_length_samples");
    check::integer_within(v.hop_length_samples, sonare_c_detail::kVocalCreateHopLengthDomain,
                          "options.hop_length_samples");
    check::greater_than(v.fmin_hz, 0.0, "options.fmin_hz");
    if (!(std::isfinite(v.fmax_hz) && v.fmax_hz > v.fmin_hz))
      check::refuse_relation({"options.fmax_hz", "options.fmin_hz"},
                             "{0} must be finite and greater than {1}");
    check::in_interval(v.yin_threshold, 0.0, false, 1.0, true, "options.yin_threshold");
    check::in_range(v.voiced_threshold, 0.0, 1.0, "options.voiced_threshold");
    check::greater_than(v.reference_hz, 0.0, "options.reference_hz");
  }
  vocal::VocalSessionCreateOptions result;
  result.output_length_samples = v.output_length_samples;
  result.render_settings.edge_fade_ms = v.edge_fade_ms;
  result.render_settings.vibrato_cutoff_hz = v.vibrato_cutoff_hz;
  result.analysis_options.segmentation_threshold_cents = v.segmentation_threshold_cents;
  result.analysis_options.minimum_note_ms = v.min_note_ms;
  if (v.frame_length_samples > INT_MAX || v.hop_length_samples > INT_MAX)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput,
                                    "analysis dimensions too large", "analysis");
  result.analysis_options.pitch.frame_length = static_cast<int>(v.frame_length_samples);
  result.analysis_options.pitch.hop_length = static_cast<int>(v.hop_length_samples);
  result.analysis_options.pitch.fmin = static_cast<float>(v.fmin_hz);
  result.analysis_options.pitch.fmax = static_cast<float>(v.fmax_hz);
  if (v.centered > 1)
    throw vocal::VocalEditException(vocal::VocalReason::kInvalidInput, "invalid centered flag",
                                    "centered");
  result.analysis_options.pitch.threshold = static_cast<float>(v.yin_threshold);
  result.analysis_options.pitch.center = v.centered != 0;
  result.analysis_options.voiced_threshold = v.voiced_threshold;
  result.analysis_options.reference_hz = v.reference_hz;
  result.limits = {v.max_history_bytes, v.max_cache_bytes, v.max_undo_depth, v.max_render_jobs};
  if (v.analysis) {
    const auto& a = *v.analysis;
    require_header(&a, "options.analysis.struct_size");
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
    result.analysis = std::move(analysis);
  }
  return result;
}
}  // namespace
#define VOCAL_SUPPORTED_BODY(...) __VA_ARGS__
#define VOCAL_ACTION(...) \
  return invoke(          \
      [&]([[maybe_unused]] const char*& expected_subject) { VOCAL_SUPPORTED_BODY(__VA_ARGS__); })
#else
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define VOCAL_SUPPORTED_BODY(...)      \
  throw sonare::SonareException(       \
      sonare::ErrorCode::NotSupported, \
      sonare_c_detail::not_compiled_in_message(sonare_c_detail::Feature::kPitchEditor))
#define VOCAL_ACTION(...) return invoke([](const char*&) { VOCAL_SUPPORTED_BODY(__VA_ARGS__); })
#endif
// A result struct is caller-versioned, so it is cleared only when its header is valid; each body
// refuses an invalid header after its earlier arguments have been checked.
#define VOCAL_OUTPUT_ACTION(out, ...)                                 \
  return invoke([&]([[maybe_unused]] const char*& expected_subject) { \
    const bool result_header_ok = result_header_valid(out);           \
    if (result_header_ok) initialize(out);                            \
    VOCAL_SUPPORTED_BODY(__VA_ARGS__);                                \
  })

SonareError sonare_vocal_session_create(const float* samples, int64_t frames, int channels,
                                        int sample_rate, const SonareVocalCreateOptions* options,
                                        SonareVocalSession** out_session) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_session);
  VOCAL_ACTION(
      auto source = import_source(samples, frames, channels, sample_rate);
      auto create_options = import_options(options); require_pointer(out_session, "out_session");
      auto session = vocal::VocalEditSession::create(std::move(source), std::move(create_options));
      *out_session = new SonareVocalSession{std::move(session)});
}
SonareError sonare_vocal_session_restore(const float* samples, int64_t frames, int channels,
                                         int sample_rate, const uint8_t* state, uint64_t size,
                                         const SonareVocalRestoreOptions* options,
                                         SonareVocalSession** out_session) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_session);
  VOCAL_ACTION(SonareVocalRestoreOptions defaults; sonare_vocal_restore_options_init(&defaults);
               const auto& v = options ? *options : defaults;
               auto source = import_source(samples, frames, channels, sample_rate);
               auto bytes = copy_input(state, size, "state");
               require_header(&v, "options.struct_size");
               require_pointer(out_session, "out_session"); const vocal::VocalSessionLimits limits{
                   v.max_history_bytes, v.max_cache_bytes, v.max_undo_depth, v.max_render_jobs};
               auto restored = vocal::VocalEditSession::restore(std::move(source), bytes, limits);
               *out_session = new SonareVocalSession{std::move(restored)});
}
SonareError sonare_vocal_session_notes(const SonareVocalSession* session,
                                       SonareVocalNotesResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(session, "session");
                      require_result_header(result_header_ok, result, "result");
                      map_notes(session->value, result));
}
SonareError sonare_vocal_draft_notes(const SonareVocalDraft* draft,
                                     SonareVocalNotesResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(draft, "draft");
                      require_result_header(result_header_ok, result, "result");
                      map_notes(*draft->value, result));
}
SonareError sonare_vocal_session_token(const SonareVocalSession* session,
                                       SonareVocalStateToken* token) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(token);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(token, "token");
               *token = map_token(session->value.token()));
}
SonareError sonare_vocal_draft_token(const SonareVocalDraft* draft, SonareVocalStateToken* token) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(token);
  VOCAL_ACTION(require_pointer(draft, "draft"); require_pointer(token, "token");
               *token = map_token(draft->value->token()));
}
SonareError sonare_vocal_session_revision(const SonareVocalSession* session, uint64_t* revision) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(revision);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(revision, "revision");
               *revision = session->value.token().committed_revision);
}
SonareError sonare_vocal_session_output_length(const SonareVocalSession* session,
                                               int64_t* samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(samples);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(samples, "samples");
               *samples = session->value.output_length_samples());
}
SonareError sonare_vocal_session_history(const SonareVocalSession* session, int* can_undo,
                                         int* can_redo) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(can_undo, can_redo);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(can_undo, "can_undo");
               require_pointer(can_redo, "can_redo"); *can_undo = session->value.can_undo();
               *can_redo = session->value.can_redo());
}
SonareError sonare_vocal_snapshot_output_length(const SonareVocalSnapshot* snapshot,
                                                int64_t* samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(samples);
  VOCAL_ACTION(require_pointer(snapshot, "snapshot"); require_pointer(samples, "samples");
               *samples = snapshot->value->data().output_length_samples);
}
SonareError sonare_vocal_session_analysis(const SonareVocalSession* session,
                                          SonareVocalAnalysisResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(
      result, require_pointer(session, "session");
      require_result_header(result_header_ok, result, "result"); auto a = session->value.analysis();
      auto descriptor = session->value.source_descriptor(); auto f0 = copy_output(a.f0_hz);
      auto voiced = copy_output(a.voiced);
      std::unique_ptr<char[]> name(sonare_c_detail::copy_string(a.algorithm_id));
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
      local.sample_rate = static_cast<int>(descriptor.sample_rate);
      local.analysis.f0_hz = f0.release(); local.analysis.voiced = voiced.release();
      local.analysis.algorithm_id = name.release(); *result = local);
}
SonareError sonare_vocal_session_capabilities(const SonareVocalSession* session,
                                              SonareVocalCapabilities* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(session, "session");
                      require_result_header(result_header_ok, result, "result");
                      const auto c = session->value.capabilities();
                      result->abi_version = c.abi_version;
                      result->profile_id = static_cast<uint32_t>(c.profile);
                      result->monophonic_only = c.monophonic_only;
                      result->analysis_cancellable = c.analysis_cancellable;
                      result->minimum_formant_shift_semitones = c.min_formant_shift_semitones;
                      result->maximum_formant_shift_semitones = c.max_formant_shift_semitones);
}
SonareError sonare_vocal_session_begin_edit(SonareVocalSession* session, uint64_t expected_revision,
                                            SonareVocalDraft** out_draft) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_draft);
  VOCAL_ACTION(
      expected_subject = "expected_revision"; require_pointer(session, "session");
      require_pointer(out_draft, "out_draft"); auto holder = std::make_unique<SonareVocalDraft>();
      holder->value = session->value.begin_edit(expected_revision); *out_draft = holder.release());
}
SonareError sonare_vocal_draft_apply(SonareVocalDraft* draft, uint64_t expected_generation,
                                     const SonareVocalOperation* operations, uint64_t count,
                                     SonareVocalEditResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(
      result, expected_subject = "expected_generation"; require_pointer(draft, "draft");
      checked_count(operations, count, "operations"); std::vector<vocal::Operation> ops;
      ops.reserve(static_cast<size_t>(count));
      for (uint64_t i = 0; i < count; ++i) ops.push_back(import_operation(operations[i]));
      require_result_header(result_header_ok, result, "result"); PreparedEditResult prepared;
      draft->value->apply(expected_generation, ops,
                          [&](const auto& change) { map_apply(change, &prepared.value); });
      prepared.publish(result));
}
SonareError sonare_vocal_draft_commit(SonareVocalDraft* draft, uint64_t expected_revision,
                                      SonareVocalEditResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(
      result, expected_subject = "expected_revision"; require_pointer(draft, "draft");
      require_result_header(result_header_ok, result, "result"); PreparedEditResult prepared;
      draft->value->commit(expected_revision,
                           [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(result));
}
SonareError sonare_vocal_draft_cancel(SonareVocalDraft* draft) {
  SONARE_C_API_ENTRY;
  VOCAL_ACTION(require_pointer(draft, "draft"); draft->value->cancel());
}
SonareError sonare_vocal_session_undo(SonareVocalSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(
      result, expected_subject = "expected_revision"; require_pointer(session, "session");
      require_result_header(result_header_ok, result, "result"); PreparedEditResult prepared;
      session->value.undo(expected_revision,
                          [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(result));
}
SonareError sonare_vocal_session_redo(SonareVocalSession* session, uint64_t expected_revision,
                                      SonareVocalEditResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(
      result, expected_subject = "expected_revision"; require_pointer(session, "session");
      require_result_header(result_header_ok, result, "result"); PreparedEditResult prepared;
      session->value.redo(expected_revision,
                          [&](const auto& change) { map_change(change, &prepared.value); });
      prepared.publish(result));
}
SonareError sonare_vocal_session_evaluate_pitch(const SonareVocalSession* session, uint32_t note_id,
                                                SonareVocalPitchResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(session, "session");
                      require_result_header(result_header_ok, result, "result");
                      map_pitch(session->value.evaluate_pitch({note_id}), result));
}
SonareError sonare_vocal_draft_evaluate_pitch(const SonareVocalDraft* draft, uint32_t note_id,
                                              SonareVocalPitchResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(draft, "draft");
                      require_result_header(result_header_ok, result, "result");
                      map_pitch(draft->value->evaluate_pitch({note_id}), result));
}
SonareError sonare_vocal_session_source_to_destination(const SonareVocalSession* session,
                                                       uint32_t note_id, double source_sample,
                                                       double* destination_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(destination_sample);
  VOCAL_ACTION(require_pointer(session, "session");
               require_pointer(destination_sample, "destination_sample");
               *destination_sample =
                   session->value.source_sample_to_destination_sample(note_id, source_sample));
}
SonareError sonare_vocal_session_destination_to_source(const SonareVocalSession* session,
                                                       uint32_t note_id, double destination_sample,
                                                       double* source_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(source_sample);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(source_sample, "source_sample");
               *source_sample =
                   session->value.destination_sample_to_source_sample(note_id, destination_sample));
}
SonareError sonare_vocal_draft_source_to_destination(const SonareVocalDraft* draft,
                                                     uint32_t note_id, double source_sample,
                                                     double* destination_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(destination_sample);
  VOCAL_ACTION(require_pointer(draft, "draft");
               require_pointer(destination_sample, "destination_sample");
               *destination_sample =
                   draft->value->source_sample_to_destination_sample(note_id, source_sample));
}
SonareError sonare_vocal_draft_destination_to_source(const SonareVocalDraft* draft,
                                                     uint32_t note_id, double destination_sample,
                                                     double* source_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(source_sample);
  VOCAL_ACTION(require_pointer(draft, "draft"); require_pointer(source_sample, "source_sample");
               *source_sample =
                   draft->value->destination_sample_to_source_sample(note_id, destination_sample));
}
SonareError sonare_vocal_session_capture_snapshot(const SonareVocalSession* session,
                                                  SonareVocalSnapshot** snapshot) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(snapshot);
  VOCAL_ACTION(require_pointer(session, "session"); require_pointer(snapshot, "snapshot");
               *snapshot = new SonareVocalSnapshot{session->value.capture_render_snapshot()});
}
SonareError sonare_vocal_draft_capture_snapshot(const SonareVocalDraft* draft,
                                                SonareVocalSnapshot** snapshot) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(snapshot);
  VOCAL_ACTION(require_pointer(draft, "draft"); require_pointer(snapshot, "snapshot");
               *snapshot = new SonareVocalSnapshot{draft->value->capture_render_snapshot()});
}
SonareError sonare_vocal_snapshot_render(const SonareVocalSnapshot* snapshot,
                                         SonareVocalRange range, uint64_t request_id,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         SonareVocalRenderResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(snapshot, "snapshot");
                      require_result_header(result_header_ok, result, "result"); map_render(
                          vocal::render_snapshot(
                              snapshot->value, {{range.start_sample, range.end_sample}, request_id},
                              cancel_probe(cancel, user_data)),
                          result));
}
SonareError sonare_vocal_render_job_begin(const SonareVocalSnapshot* snapshot,
                                          SonareVocalRange range, uint64_t request_id,
                                          SonareVocalRenderJob** job) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(job);
  VOCAL_ACTION(require_pointer(snapshot, "snapshot"); require_pointer(job, "job");
               auto holder = std::make_unique<SonareVocalRenderJob>();
               holder->value = std::make_unique<vocal::VocalRenderJob>(
                   snapshot->value,
                   vocal::VocalRenderRequest{{range.start_sample, range.end_sample}, request_id});
               *job = holder.release());
}
SonareError sonare_vocal_render_job_next(SonareVocalRenderJob* job,
                                         SonareVocalCancelCallback cancel, void* user_data,
                                         int* complete) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(complete);
  VOCAL_ACTION(require_pointer(job, "job"); require_pointer(complete, "complete");
               job->value->next(cancel_probe(cancel, user_data));
               const auto state = job->value->progress().state;
               if (state == vocal::VocalRenderJobState::kAborted) throw vocal::VocalEditException(
                   vocal::VocalReason::kCancelled, "render job aborted", "render");
               *complete = state == vocal::VocalRenderJobState::kComplete ? 1 : 0);
}
SonareError sonare_vocal_render_job_finalize(SonareVocalRenderJob* job,
                                             SonareVocalCancelCallback cancel, void* user_data,
                                             SonareVocalRenderResult* result) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(result, require_pointer(job, "job");
                      require_result_header(result_header_ok, result, "result");
                      map_render(job->value->finalize(cancel_probe(cancel, user_data)), result));
}
SonareError sonare_vocal_session_export_state(const SonareVocalSession* session,
                                              SonareVocalStateBytes* state) {
  SONARE_C_API_ENTRY;
  VOCAL_OUTPUT_ACTION(state, require_pointer(session, "session");
                      require_result_header(result_header_ok, state, "state");
                      auto bytes = session->value.export_state(); auto data = copy_output(bytes);
                      state->size = bytes.size(); state->data = data.release());
}

void sonare_vocal_session_destroy(SonareVocalSession* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
void sonare_vocal_draft_destroy(SonareVocalDraft* s) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  delete s;
#else
  (void)s;
#endif
}
void sonare_vocal_snapshot_destroy(SonareVocalSnapshot* s) {
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
