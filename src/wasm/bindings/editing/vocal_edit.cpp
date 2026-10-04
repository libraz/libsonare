/// @file vocal_edit.cpp
/// @brief WASM value facade for the C vocal-edit ABI.
///
/// The C surface owns every native object and every result buffer.  This file
/// deliberately keeps only opaque integer handles on the JavaScript side and
/// copies result arrays before invoking the matching C free function.  It is
/// consequently safe to structured-clone all returned values and to move the
/// value protocol to a dedicated Worker.

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <sonare/sonare_c_vocal_edit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "util/exception.h"
#include "wasm/bindings/common/common.h"

namespace {

using emscripten::val;
using sonare::ErrorCode;
using sonare::SonareException;

using Handle = std::uintptr_t;

thread_local uint64_t vocal_error_sequence = 0;

val vectorToFloat64Array(const double* data, std::size_t count) {
  val result = val::global("Float64Array").new_(count);
  if (count != 0) {
    val view = val(emscripten::typed_memory_view(count, data));
    result.call<void>("set", view);
  }
  return result;
}

[[noreturn]] void throwVocalError(SonareError error, const char* context) {
  SonareVocalErrorDetail structured{};
  sonare_vocal_error_detail_init(&structured);
  sonare_vocal_last_error_detail(&structured);
  vocal_error_sequence =
      vocal_error_sequence == std::numeric_limits<uint64_t>::max() ? 0 : vocal_error_sequence + 1;
  std::string message = context == nullptr ? "vocal edit operation failed" : context;
  message += ": ";
  const char* detail = sonare_last_error_message();
  const char* fallback = sonare_error_message(error);
  message += detail != nullptr && detail[0] != '\0' ? detail : fallback;
  if ((detail == nullptr || detail[0] == '\0') && structured.field[0] != '\0') {
    message += " (";
    message += structured.field;
    message += ")";
  }
  throw SonareException(static_cast<ErrorCode>(error), message);
}

template <typename F>
void check(F&& function, const char* context) {
  const SonareError error = function();
  if (error != SONARE_OK) throwVocalError(error, context);
}

template <typename T>
T* handlePointer(double value, const char* field) {
  const double addressSpaceLimit = std::ldexp(1.0, sizeof(Handle) * 8);
  if (!std::isfinite(value) || value <= 0.0 || std::floor(value) != value ||
      value >= addressSpaceLimit) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(field) + " must be a handle");
  }
  return reinterpret_cast<T*>(static_cast<Handle>(value));
}

template <typename T>
double handleValue(T* value) {
  return static_cast<double>(reinterpret_cast<Handle>(value));
}

bool absent(const val& value) { return value.isUndefined() || value.isNull(); }

val property(const val& object, const char* key) { return object[key]; }

std::string stringPropertyStrict(const val& object, const char* key, const char* fallback) {
  const val value = property(object, key);
  if (absent(value)) return fallback == nullptr ? std::string{} : std::string(fallback);
  if (value.typeOf().as<std::string>() != "string") {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be a string");
  }
  return value.as<std::string>();
}

bool booleanProperty(const val& object, const char* key, bool fallback) {
  const val value = property(object, key);
  if (absent(value)) return fallback;
  if (value.typeOf().as<std::string>() != "boolean") {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be boolean");
  }
  return value.as<bool>();
}

uint32_t nonZeroId(uint32_t id, const char* key) {
  if (id == 0) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be non-zero");
  }
  return id;
}

std::vector<float> floatArray(const val& value, const char* field, bool non_negative = false) {
  const std::size_t count = wasmArrayLikeLength(value, field);
  std::vector<float> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    const val item = value[static_cast<unsigned>(i)];
    const double number = item.typeOf().as<std::string>() == "number"
                              ? item.as<double>()
                              : std::numeric_limits<double>::quiet_NaN();
    const double minimum =
        non_negative ? 0.0 : -static_cast<double>(std::numeric_limits<float>::max());
    const double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(number) || number < minimum || number > maximum) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(field) + " must contain finite numeric values");
    }
    result[i] = static_cast<float>(number);
  }
  return result;
}

std::vector<uint8_t> byteArray(const val& value, const char* field) {
  const std::size_t count = wasmArrayLikeLength(value, field);
  std::vector<uint8_t> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    const val item = value[static_cast<unsigned>(i)];
    if (item.typeOf().as<std::string>() != "number") {
      throw SonareException(ErrorCode::InvalidParameter, std::string(field) + " must be bytes");
    }
    const double number = item.as<double>();
    if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 255.0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(field) + " must contain bytes");
    }
    result[i] = static_cast<uint8_t>(number);
  }
  return result;
}

std::vector<uint32_t> idArray(const val& value, const char* field) {
  const std::size_t count = wasmArrayLikeLength(value, field);
  std::vector<uint32_t> result(count);
  for (std::size_t i = 0; i < count; ++i) {
    const val item = value[static_cast<unsigned>(i)];
    const uint32_t id = checkedUintFromVal(item, field);
    if (id == 0) {
      throw SonareException(ErrorCode::InvalidParameter, std::string(field) + " contains zero id");
    }
    result[i] = id;
  }
  return result;
}

void validateSource(const std::vector<float>& source) {
  if (source.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "samples must not be empty");
  }
  for (float sample : source) {
    if (!std::isfinite(sample)) {
      throw SonareException(ErrorCode::InvalidParameter, "samples must be finite");
    }
  }
}

struct EditStorage {
  std::vector<SonareVocalPitchPoint> points;
  std::vector<float> envelope;
};

SonareVocalNoteEdit editFromVal(const val& object, EditStorage* storage) {
  if (object.isNull() || object.isUndefined() || object.typeOf().as<std::string>() != "object") {
    throw SonareException(ErrorCode::InvalidParameter, "edit must be an object");
  }
  SonareVocalNoteEdit result;
  sonare_vocal_note_edit_init(&result);
  const val pitch = property(object, "pitch");
  if (pitch.isNull() || pitch.isUndefined() || pitch.typeOf().as<std::string>() != "object") {
    throw SonareException(ErrorCode::InvalidParameter, "edit.pitch must be an object");
  }
  const val target = property(pitch, "target");
  if (target.isNull() || target.isUndefined() || target.typeOf().as<std::string>() != "object") {
    throw SonareException(ErrorCode::InvalidParameter, "edit.pitch.target must be an object");
  }
  const std::string targetMode = stringPropertyStrict(target, "mode", "none");
  if (targetMode == "none") {
    result.target_mode = SONARE_VOCAL_TARGET_NONE;
  } else if (targetMode == "center") {
    result.target_mode = SONARE_VOCAL_TARGET_CENTER;
    result.target_midi = typedDoubleProperty(target, "midi", 0.0);
  } else if (targetMode == "curve") {
    result.target_mode = SONARE_VOCAL_TARGET_CURVE;
    const val points = property(target, "points");
    const std::size_t count = wasmArrayLikeLength(points, "target.points");
    if (count < 2) {
      throw SonareException(ErrorCode::InvalidParameter, "target.points must contain two points");
    }
    storage->points.reserve(count);
    double previous = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < count; ++i) {
      const val point = points[static_cast<unsigned>(i)];
      const double source = typedDoubleProperty(point, "sourceSample", 0.0);
      const double midi = typedDoubleProperty(point, "midi", 0.0);
      if (source <= previous) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "target.points must be strictly increasing");
      }
      storage->points.push_back({source, midi});
      previous = source;
    }
    result.target_points = storage->points.data();
    result.target_point_count = storage->points.size();
  } else {
    throw SonareException(ErrorCode::InvalidParameter, "unsupported pitch target mode");
  }
  result.amount = typedDoubleProperty(pitch, "amount", result.amount);
  result.speed_ms = typedDoubleProperty(pitch, "speedMs", result.speed_ms);
  result.max_correction_semitones =
      typedDoubleProperty(pitch, "maxCorrectionSemitones", result.max_correction_semitones);
  result.transpose_semitones =
      typedDoubleProperty(pitch, "transposeSemitones", result.transpose_semitones);
  result.drift_scale = typedDoubleProperty(pitch, "driftScale", result.drift_scale);
  result.vibrato_scale = typedDoubleProperty(pitch, "vibratoScale", result.vibrato_scale);
  result.destination_start_sample = int64Property(object, "destinationStartSample", 0);
  result.destination_length_samples = int64Property(object, "destinationLengthSamples", 0);
  result.gain_db = typedDoubleProperty(object, "gainDb", 0.0);
  const val muted = property(object, "muted");
  if (!absent(muted)) {
    if (muted.typeOf().as<std::string>() != "boolean") {
      throw SonareException(ErrorCode::InvalidParameter, "edit.muted must be boolean");
    }
    result.muted = muted.as<bool>() ? 1u : 0u;
  }
  const val formant = property(object, "formant");
  if (!absent(formant)) {
    const std::string mode = stringPropertyStrict(formant, "mode", "preserve");
    if (mode == "preserve")
      result.formant_mode = SONARE_VOCAL_FORMANT_PRESERVE;
    else if (mode == "shift")
      result.formant_mode = SONARE_VOCAL_FORMANT_SHIFT;
    else
      throw SonareException(ErrorCode::InvalidParameter, "unsupported formant mode");
    result.formant_shift_semitones = typedDoubleProperty(formant, "shiftSemitones", 0.0);
  }
  const val envelope = property(object, "amplitudeEnvelope");
  if (!absent(envelope)) {
    storage->envelope = floatArray(envelope, "edit.amplitudeEnvelope", true);
    result.amplitude_envelope = storage->envelope.data();
    result.amplitude_envelope_count = storage->envelope.size();
  }
  return result;
}

int64_t nonNegativeWindow(int64_t value, const char* field) {
  if (value < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(field) + " must be a non-negative integer");
  }
  return value;
}

SonareVocalTransition transitionFromVal(const val& object) {
  if (object.isNull() || object.isUndefined() || object.typeOf().as<std::string>() != "object") {
    throw SonareException(ErrorCode::InvalidParameter, "transition must be an object");
  }
  SonareVocalTransition result{};
  result.struct_size = sizeof(result);
  result.schema_version = SONARE_VOCAL_EDIT_API_VERSION;
  result.left_note_id =
      nonZeroId(checkedUintFromVal(property(object, "leftNoteId"), "leftNoteId"), "leftNoteId");
  result.right_note_id =
      nonZeroId(checkedUintFromVal(property(object, "rightNoteId"), "rightNoteId"), "rightNoteId");
  result.left_window_samples = nonNegativeWindow(int64Property(object, "leftWindowSamples", -1),
                                                 "transition.leftWindowSamples");
  result.right_window_samples = nonNegativeWindow(int64Property(object, "rightWindowSamples", -1),
                                                  "transition.rightWindowSamples");
  result.strength =
      typedDoubleProperty(object, "strength", std::numeric_limits<double>::quiet_NaN());
  if (!(result.strength >= 0.0 && result.strength <= 1.0)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "transition.strength must be a number in [0, 1]");
  }
  if (stringPropertyStrict(object, "curve", "") != "smoothstep") {
    throw SonareException(ErrorCode::InvalidParameter, "transition.curve must be smoothstep");
  }
  return result;
}

struct OperationStorage {
  EditStorage edit;
  std::vector<uint32_t> ids;
};

SonareVocalOperation operationFromVal(const val& object, OperationStorage* storage) {
  SonareVocalOperation result;
  sonare_vocal_operation_init(&result);
  const std::string kind = stringPropertyStrict(object, "kind", "");
  if (kind == "setEdit") {
    result.kind = SONARE_VOCAL_SET_EDIT;
    result.note_id = nonZeroId(checkedUintFromVal(property(object, "noteId"), "noteId"), "noteId");
    result.edit = editFromVal(property(object, "edit"), &storage->edit);
  } else if (kind == "setSourceSpan") {
    result.kind = SONARE_VOCAL_SET_SOURCE_SPAN;
    result.note_id = nonZeroId(checkedUintFromVal(property(object, "noteId"), "noteId"), "noteId");
    result.source_start_sample = int64Property(object, "sourceStartSample", 0);
    result.source_end_sample = int64Property(object, "sourceEndSample", 0);
    result.destination_start_sample = int64Property(object, "destinationStartSample", 0);
    result.destination_length_samples = int64Property(object, "destinationLengthSamples", 0);
  } else if (kind == "split") {
    result.kind = SONARE_VOCAL_SPLIT;
    result.note_id = nonZeroId(checkedUintFromVal(property(object, "noteId"), "noteId"), "noteId");
    result.cut_source_sample = int64Property(object, "sourceSample", 0);
  } else if (kind == "merge") {
    result.kind = SONARE_VOCAL_MERGE;
    storage->ids = idArray(property(object, "noteIds"), "noteIds");
    result.note_ids = storage->ids.data();
    result.note_id_count = storage->ids.size();
    const std::string policy = stringPropertyStrict(object, "policy", "preserve");
    if (policy == "preserve")
      result.merge_policy = SONARE_VOCAL_MERGE_PRESERVE;
    else if (policy == "reset")
      result.merge_policy = SONARE_VOCAL_MERGE_RESET;
    else
      throw SonareException(ErrorCode::InvalidParameter, "unsupported merge policy");
  } else if (kind == "setTransition") {
    result.kind = SONARE_VOCAL_SET_TRANSITION;
    result.transition = transitionFromVal(property(object, "transition"));
  } else if (kind == "removeTransition") {
    result.kind = SONARE_VOCAL_REMOVE_TRANSITION;
    result.transition.left_note_id =
        nonZeroId(checkedUintFromVal(property(object, "leftNoteId"), "leftNoteId"), "leftNoteId");
    result.transition.right_note_id = nonZeroId(
        checkedUintFromVal(property(object, "rightNoteId"), "rightNoteId"), "rightNoteId");
  } else if (kind == "reset") {
    result.kind = SONARE_VOCAL_RESET;
    storage->ids = idArray(property(object, "noteIds"), "noteIds");
    result.note_ids = storage->ids.data();
    result.note_id_count = storage->ids.size();
  } else {
    throw SonareException(ErrorCode::InvalidParameter, "unsupported vocal edit operation");
  }
  return result;
}

void readSessionLimits(const val& object, uint64_t* max_history_bytes, uint64_t* max_cache_bytes,
                       uint32_t* max_undo_depth, uint32_t* max_render_jobs) {
  const val limits = property(object, "limits");
  if (absent(limits)) return;
  *max_history_bytes = decimalUint64Property(limits, "maxHistoryBytes", *max_history_bytes);
  *max_cache_bytes = decimalUint64Property(limits, "maxCacheBytes", *max_cache_bytes);
  *max_undo_depth = uintProperty(limits, "maxUndoDepth", *max_undo_depth);
  *max_render_jobs = uintProperty(limits, "maxRenderJobs", *max_render_jobs);
}

struct CreateStorage {
  SonareVocalCreateOptions options{};
  SonareVocalAnalysis analysis{};
  std::vector<float> f0;
  std::vector<uint8_t> voiced;
  std::string algorithm;
};

SonareVocalCreateOptions optionsFromVal(const val& object, CreateStorage* storage) {
  sonare_vocal_create_options_init(&storage->options);
  if (absent(object)) return storage->options;
  storage->options.output_length_samples = int64Property(object, "outputLengthSamples", 0);
  storage->options.edge_fade_ms =
      typedDoubleProperty(object, "edgeFadeMs", storage->options.edge_fade_ms);
  storage->options.vibrato_cutoff_hz =
      typedDoubleProperty(object, "vibratoCutoffHz", storage->options.vibrato_cutoff_hz);
  storage->options.segmentation_threshold_cents = typedDoubleProperty(
      object, "segmentationThresholdCents", storage->options.segmentation_threshold_cents);
  storage->options.min_note_ms =
      typedDoubleProperty(object, "minNoteMs", storage->options.min_note_ms);
  storage->options.frame_length_samples =
      uintProperty(object, "frameLengthSamples", storage->options.frame_length_samples);
  storage->options.hop_length_samples =
      uintProperty(object, "hopLengthSamples", storage->options.hop_length_samples);
  storage->options.fmin_hz = typedDoubleProperty(object, "fminHz", storage->options.fmin_hz);
  storage->options.fmax_hz = typedDoubleProperty(object, "fmaxHz", storage->options.fmax_hz);
  storage->options.yin_threshold =
      typedDoubleProperty(object, "yinThreshold", storage->options.yin_threshold);
  storage->options.voiced_threshold =
      typedDoubleProperty(object, "voicedThreshold", storage->options.voiced_threshold);
  storage->options.centered =
      booleanProperty(object, "centered", storage->options.centered != 0) ? 1u : 0u;
  storage->options.reference_hz =
      typedDoubleProperty(object, "referenceHz", storage->options.reference_hz);
  readSessionLimits(object, &storage->options.max_history_bytes, &storage->options.max_cache_bytes,
                    &storage->options.max_undo_depth, &storage->options.max_render_jobs);
  const val analysis = property(object, "analysis");
  if (!absent(analysis)) {
    sonare_vocal_analysis_init(&storage->analysis);
    storage->analysis.frame_origin_sample = typedDoubleProperty(analysis, "frameOriginSample", 0.0);
    storage->analysis.samples_per_frame = typedDoubleProperty(analysis, "samplesPerFrame", 0.0);
    storage->analysis.frame_length_samples = uintProperty(analysis, "frameLengthSamples", 0);
    // A supplied analysis is authoritative for fields it carries; omitted
    // optional settings inherit the create options, matching the other
    // language facades.
    storage->analysis.fmin_hz = typedDoubleProperty(analysis, "fminHz", storage->options.fmin_hz);
    storage->analysis.fmax_hz = typedDoubleProperty(analysis, "fmaxHz", storage->options.fmax_hz);
    storage->analysis.yin_threshold =
        typedDoubleProperty(analysis, "yinThreshold", storage->options.yin_threshold);
    storage->analysis.voiced_threshold =
        typedDoubleProperty(analysis, "voicedThreshold", storage->options.voiced_threshold);
    storage->analysis.centered =
        booleanProperty(analysis, "centered", storage->options.centered != 0) ? 1u : 0u;
    storage->analysis.segmentation_threshold_cents = typedDoubleProperty(
        analysis, "segmentationThresholdCents", storage->options.segmentation_threshold_cents);
    storage->analysis.min_note_ms =
        typedDoubleProperty(analysis, "minNoteMs", storage->options.min_note_ms);
    storage->analysis.reference_hz =
        typedDoubleProperty(analysis, "referenceHz", storage->options.reference_hz);
    storage->f0 = floatArray(property(analysis, "f0Hz"), "analysis.f0Hz");
    storage->voiced = byteArray(property(analysis, "voiced"), "analysis.voiced");
    if (storage->f0.size() != storage->voiced.size()) {
      throw SonareException(ErrorCode::InvalidParameter, "analysis arrays must have equal length");
    }
    storage->algorithm = stringPropertyStrict(analysis, "algorithmId", "host");
    if (storage->algorithm.find('\0') != std::string::npos) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "analysis.algorithmId must not contain NUL");
    }
    storage->analysis.algorithm_version = uintProperty(analysis, "algorithmVersion", 1);
    storage->analysis.f0_hz = storage->f0.data();
    storage->analysis.voiced = storage->voiced.data();
    storage->analysis.frame_count = storage->f0.size();
    storage->analysis.algorithm_id = storage->algorithm.c_str();
    storage->options.analysis = &storage->analysis;
  }
  return storage->options;
}

val tokenToVal(const SonareVocalStateToken& token) {
  val out = val::object();
  out.set("sessionEpoch", std::to_string(token.session_epoch));
  out.set("revision", std::to_string(token.revision));
  out.set("draftId", std::to_string(token.draft_id));
  out.set("generation", std::to_string(token.generation));
  out.set("requestId", std::to_string(token.request_id));
  out.set("profileId", token.profile_id);
  return out;
}

val rangeToVal(const SonareVocalRange& range) {
  val out = val::object();
  out.set("startSample", static_cast<double>(range.start_sample));
  out.set("endSample", static_cast<double>(range.end_sample));
  return out;
}

val editToVal(const SonareVocalNoteEdit& edit) {
  val pitch = val::object();
  const char* modes[] = {"none", "center", "curve"};
  pitch.set("target", val::object());
  val target = val::object();
  target.set("mode", modes[std::min<uint32_t>(edit.target_mode, 2u)]);
  if (edit.target_mode == SONARE_VOCAL_TARGET_CENTER) target.set("midi", edit.target_midi);
  if (edit.target_mode == SONARE_VOCAL_TARGET_CURVE) {
    val points = val::array();
    for (uint64_t i = 0; i < edit.target_point_count; ++i) {
      val point = val::object();
      point.set("sourceSample", edit.target_points[i].source_sample);
      point.set("midi", edit.target_points[i].midi);
      points.set(static_cast<unsigned>(i), point);
    }
    target.set("points", points);
  }
  pitch.set("target", target);
  pitch.set("amount", edit.amount);
  pitch.set("speedMs", edit.speed_ms);
  pitch.set("maxCorrectionSemitones", edit.max_correction_semitones);
  pitch.set("transposeSemitones", edit.transpose_semitones);
  pitch.set("driftScale", edit.drift_scale);
  pitch.set("vibratoScale", edit.vibrato_scale);
  val out = val::object();
  out.set("pitch", pitch);
  out.set("destinationStartSample", static_cast<double>(edit.destination_start_sample));
  out.set("destinationLengthSamples", static_cast<double>(edit.destination_length_samples));
  out.set("gainDb", edit.gain_db);
  out.set("muted", edit.muted != 0);
  if (edit.amplitude_envelope != nullptr && edit.amplitude_envelope_count != 0) {
    out.set("amplitudeEnvelope",
            vectorToFloat32Array(edit.amplitude_envelope, edit.amplitude_envelope_count));
  } else {
    out.set("amplitudeEnvelope", val::global("Float32Array").new_(0));
  }
  val formant = val::object();
  formant.set("mode", edit.formant_mode == SONARE_VOCAL_FORMANT_SHIFT ? "shift" : "preserve");
  formant.set("shiftSemitones", edit.formant_shift_semitones);
  out.set("formant", formant);
  return out;
}

val noteToVal(const SonareVocalNote& note) {
  val out = val::object();
  out.set("id", note.id);
  out.set("sourceStartSample", static_cast<double>(note.source_start_sample));
  out.set("sourceEndSample", static_cast<double>(note.source_end_sample));
  out.set("analysisFrameStart", std::to_string(note.analysis_frame_start));
  out.set("analysisFrameEnd", std::to_string(note.analysis_frame_end));
  out.set("hasPitch", note.has_pitch != 0);
  out.set("medianHz", note.median_hz);
  out.set("centerMidi", note.center_midi);
  out.set("f0Stability", note.f0_stability);
  out.set("amplitude", vectorToFloat32Array(note.amplitude, note.amplitude_count));
  out.set("edit", editToVal(note.edit));
  return out;
}

val notesResultToVal(const SonareVocalNotesResult& result) {
  val out = val::object();
  val notes = val::array();
  for (uint64_t i = 0; i < result.note_count; ++i)
    notes.set(static_cast<unsigned>(i), noteToVal(result.notes[i]));
  val transitions = val::array();
  for (uint64_t i = 0; i < result.transition_count; ++i) {
    const auto& source = result.transitions[i];
    val transition = val::object();
    transition.set("leftNoteId", source.left_note_id);
    transition.set("rightNoteId", source.right_note_id);
    transition.set("leftWindowSamples", static_cast<double>(source.left_window_samples));
    transition.set("rightWindowSamples", static_cast<double>(source.right_window_samples));
    transition.set("strength", source.strength);
    transition.set("curve", "smoothstep");
    transitions.set(static_cast<unsigned>(i), transition);
  }
  out.set("notes", notes);
  out.set("transitions", transitions);
  return out;
}

struct NotesGuard {
  SonareVocalNotesResult value{};
  NotesGuard() { sonare_vocal_notes_result_init(&value); }
  ~NotesGuard() { sonare_vocal_free_notes(&value); }
};
struct AnalysisGuard {
  SonareVocalAnalysisResult value{};
  AnalysisGuard() { sonare_vocal_analysis_result_init(&value); }
  ~AnalysisGuard() { sonare_vocal_free_analysis(&value); }
};
struct EditResultGuard {
  SonareVocalEditResult value{};
  EditResultGuard() { sonare_vocal_edit_result_init(&value); }
  ~EditResultGuard() { sonare_vocal_free_edit_result(&value); }
};
struct PitchGuard {
  SonareVocalPitchResult value{};
  PitchGuard() { sonare_vocal_pitch_result_init(&value); }
  ~PitchGuard() { sonare_vocal_free_pitch_result(&value); }
};
struct RenderGuard {
  SonareVocalRenderResult value{};
  RenderGuard() { sonare_vocal_render_result_init(&value); }
  ~RenderGuard() { sonare_vocal_free_render_result(&value); }
};
struct StateGuard {
  SonareVocalStateBytes value{};
  StateGuard() { sonare_vocal_state_bytes_init(&value); }
  ~StateGuard() { sonare_vocal_free_state_bytes(&value); }
};

val editResultToVal(const SonareVocalEditResult& result) {
  val out = val::object();
  out.set("token", tokenToVal(result.token));
  val dirty = val::array();
  for (uint64_t i = 0; i < result.dirty_range_count; ++i)
    dirty.set(static_cast<unsigned>(i), rangeToVal(result.dirty_ranges[i]));
  val changes = val::array();
  for (uint64_t i = 0; i < result.id_change_count; ++i) {
    const auto& change = result.id_changes[i];
    val item = val::object();
    item.set("operationIndex", change.operation_index);
    item.set("retiredId", change.retired_id);
    val ids = val::array();
    if (change.first_new_id != 0) ids.set(0, change.first_new_id);
    if (change.second_new_id != 0) ids.set(1, change.second_new_id);
    item.set("newIds", ids);
    changes.set(static_cast<unsigned>(i), item);
  }
  out.set("dirtyRanges", dirty);
  out.set("idChanges", changes);
  return out;
}

val analysisResultToVal(const SonareVocalAnalysisResult& result) {
  val out = val::object();
  out.set("frameOriginSample", result.analysis.frame_origin_sample);
  out.set("samplesPerFrame", result.analysis.samples_per_frame);
  out.set("frameLengthSamples", result.analysis.frame_length_samples);
  out.set("f0Hz", vectorToFloat32Array(result.analysis.f0_hz, result.analysis.frame_count));
  out.set("voiced", vectorToUint8Array(result.analysis.voiced, result.analysis.frame_count));
  out.set("algorithmId",
          result.analysis.algorithm_id == nullptr ? "" : result.analysis.algorithm_id);
  out.set("algorithmVersion", result.analysis.algorithm_version);
  out.set("sourceLengthSamples", static_cast<double>(result.source_length_samples));
  out.set("sampleRate", result.sample_rate);
  out.set("sourceSha256", result.source_sha256);
  out.set("analysisSha256", result.analysis_sha256);
  out.set("fminHz", result.analysis.fmin_hz);
  out.set("fmaxHz", result.analysis.fmax_hz);
  out.set("yinThreshold", result.analysis.yin_threshold);
  out.set("voicedThreshold", result.analysis.voiced_threshold);
  out.set("centered", result.analysis.centered != 0);
  out.set("segmentationThresholdCents", result.analysis.segmentation_threshold_cents);
  out.set("minNoteMs", result.analysis.min_note_ms);
  out.set("referenceHz", result.analysis.reference_hz);
  return out;
}

val pitchResultToVal(const SonareVocalPitchResult& result) {
  val out = val::object();
  out.set("sourceSamples", vectorToFloat64Array(result.source_samples, result.frame_count));
  out.set("measuredMidi", vectorToFloat64Array(result.measured_midi, result.frame_count));
  out.set("targetMidi", vectorToFloat64Array(result.target_midi, result.frame_count));
  out.set("effectiveMidi", vectorToFloat64Array(result.effective_midi, result.frame_count));
  out.set("voiced", vectorToUint8Array(result.voiced, result.frame_count));
  out.set("hasTarget", vectorToUint8Array(result.has_target, result.frame_count));
  return out;
}

val renderResultToVal(const SonareVocalRenderResult& result) {
  val out = val::object();
  out.set("samples",
          vectorToFloat32Array(
              result.samples, static_cast<std::size_t>(std::max<int64_t>(0, result.sample_count))));
  out.set("startSample", static_cast<double>(result.start_sample));
  out.set("token", tokenToVal(result.token));
  val ranges = val::array();
  for (uint64_t i = 0; i < result.processed_range_count; ++i)
    ranges.set(static_cast<unsigned>(i), rangeToVal(result.processed_ranges[i]));
  out.set("processedRanges", ranges);
  out.set("cacheHitUnits", std::to_string(result.cache_hit_units));
  out.set("dryPassedFrames", std::to_string(result.dry_passed_frames));
  out.set("limitedCorrectionFrames", std::to_string(result.limited_correction_frames));
  return out;
}

val capabilitiesToVal(const SonareVocalCapabilities& capabilities) {
  val out = val::object();
  out.set("apiVersion", capabilities.api_version);
  out.set("profileId", capabilities.profile_id);
  out.set("monophonicOnly", capabilities.monophonic_only != 0);
  out.set("analysisCancellable", capabilities.analysis_cancellable != 0);
  out.set("minimumFormantShiftSemitones", capabilities.minimum_formant_shift_semitones);
  out.set("maximumFormantShiftSemitones", capabilities.maximum_formant_shift_semitones);
  return out;
}

val js_vocal_edit_session_create(val samples, const val& sample_rate_value, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_value, "sampleRate");
  std::vector<float> source = float32ArrayToVector(samples);
  validateSource(source);
  CreateStorage storage;
  const SonareVocalCreateOptions parsed = optionsFromVal(options, &storage);
  SonareVocalEditSession* session = nullptr;
  check(
      [&] {
        return sonare_vocal_session_create(source.data(), static_cast<int64_t>(source.size()), 1,
                                           sample_rate, &parsed, &session);
      },
      "vocal session create");
  return val(handleValue(session));
}

double js_vocal_edit_session_restore(val samples, const val& sample_rate_value, val state,
                                     val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_value, "sampleRate");
  std::vector<float> source = float32ArrayToVector(samples);
  validateSource(source);
  std::vector<uint8_t> bytes = byteArray(state, "state");
  SonareVocalRestoreOptions parsed{};
  sonare_vocal_restore_options_init(&parsed);
  if (!absent(options)) {
    readSessionLimits(options, &parsed.max_history_bytes, &parsed.max_cache_bytes,
                      &parsed.max_undo_depth, &parsed.max_render_jobs);
  }
  SonareVocalEditSession* session = nullptr;
  check(
      [&] {
        return sonare_vocal_session_restore(source.data(), static_cast<int64_t>(source.size()), 1,
                                            sample_rate, bytes.data(), bytes.size(), &parsed,
                                            &session);
      },
      "vocal session restore");
  return handleValue(session);
}

void js_vocal_edit_session_destroy(double handle) {
  if (handle == 0.0) return;
  sonare_vocal_session_destroy(handlePointer<SonareVocalEditSession>(handle, "session"));
}

val js_vocal_edit_session_notes(double handle) {
  NotesGuard result;
  check(
      [&] {
        return sonare_vocal_session_notes(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &result.value);
      },
      "vocal session notes");
  return notesResultToVal(result.value);
}

val js_vocal_edit_draft_notes(double handle) {
  NotesGuard result;
  check(
      [&] {
        return sonare_vocal_draft_notes(handlePointer<const SonareVocalEditDraft>(handle, "draft"),
                                        &result.value);
      },
      "vocal draft notes");
  return notesResultToVal(result.value);
}

val js_vocal_edit_session_token(double handle) {
  SonareVocalStateToken token{};
  check(
      [&] {
        return sonare_vocal_session_token(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &token);
      },
      "vocal session token");
  return tokenToVal(token);
}

val js_vocal_edit_draft_token(double handle) {
  SonareVocalStateToken token{};
  check(
      [&] {
        return sonare_vocal_draft_token(handlePointer<const SonareVocalEditDraft>(handle, "draft"),
                                        &token);
      },
      "vocal draft token");
  return tokenToVal(token);
}

val js_vocal_edit_session_analysis(double handle) {
  AnalysisGuard result;
  check(
      [&] {
        return sonare_vocal_session_analysis(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &result.value);
      },
      "vocal session analysis");
  return analysisResultToVal(result.value);
}

val js_vocal_edit_session_capabilities(double handle) {
  SonareVocalCapabilities value{};
  sonare_vocal_capabilities_init(&value);
  check(
      [&] {
        return sonare_vocal_session_capabilities(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &value);
      },
      "vocal session capabilities");
  return capabilitiesToVal(value);
}

double js_vocal_edit_session_output_length(double handle) {
  int64_t samples = 0;
  check(
      [&] {
        return sonare_vocal_session_output_length(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &samples);
      },
      "vocal session output length");
  return static_cast<double>(samples);
}

val js_vocal_edit_session_history(double handle) {
  int can_undo = 0;
  int can_redo = 0;
  check(
      [&] {
        return sonare_vocal_session_history(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &can_undo, &can_redo);
      },
      "vocal session history");
  val out = val::object();
  out.set("canUndo", can_undo != 0);
  out.set("canRedo", can_redo != 0);
  return out;
}

val js_vocal_edit_session_begin_edit(double handle, const val& revision_value) {
  const uint64_t expected = checkedDecimalUint64FromVal(revision_value, "expectedRevision");
  SonareVocalEditDraft* draft = nullptr;
  check(
      [&] {
        return sonare_vocal_session_begin_edit(
            handlePointer<SonareVocalEditSession>(handle, "session"), expected, &draft);
      },
      "vocal begin edit");
  return val(handleValue(draft));
}

val js_vocal_edit_draft_apply(double handle, const val& generation_value, val operations) {
  const std::size_t count = wasmArrayLikeLength(operations, "operations");
  std::vector<SonareVocalOperation> parsed(count);
  std::vector<OperationStorage> storage(count);
  for (std::size_t i = 0; i < count; ++i)
    parsed[i] = operationFromVal(operations[static_cast<unsigned>(i)], &storage[i]);
  EditResultGuard result;
  check(
      [&] {
        return sonare_vocal_draft_apply(
            handlePointer<SonareVocalEditDraft>(handle, "draft"),
            checkedDecimalUint64FromVal(generation_value, "expectedGeneration"), parsed.data(),
            parsed.size(), &result.value);
      },
      "vocal draft apply");
  return editResultToVal(result.value);
}

val js_vocal_edit_draft_commit(double handle, const val& revision_value) {
  EditResultGuard result;
  check(
      [&] {
        return sonare_vocal_draft_commit(
            handlePointer<SonareVocalEditDraft>(handle, "draft"),
            checkedDecimalUint64FromVal(revision_value, "expectedRevision"), &result.value);
      },
      "vocal draft commit");
  return editResultToVal(result.value);
}

void js_vocal_edit_draft_cancel(double handle) {
  check(
      [&] {
        return sonare_vocal_draft_cancel(handlePointer<SonareVocalEditDraft>(handle, "draft"));
      },
      "vocal draft cancel");
}

void js_vocal_edit_draft_destroy(double handle) {
  if (handle != 0.0)
    sonare_vocal_draft_destroy(handlePointer<SonareVocalEditDraft>(handle, "draft"));
}

val js_vocal_edit_session_apply_history(double handle, const val& revision_value, bool redo) {
  EditResultGuard result;
  check(
      [&] {
        const auto session = handlePointer<SonareVocalEditSession>(handle, "session");
        const uint64_t expected = checkedDecimalUint64FromVal(revision_value, "expectedRevision");
        return redo ? sonare_vocal_session_redo(session, expected, &result.value)
                    : sonare_vocal_session_undo(session, expected, &result.value);
      },
      redo ? "vocal redo" : "vocal undo");
  return editResultToVal(result.value);
}

val js_vocal_edit_evaluate_pitch(double handle, const val& note_id_value, bool draft) {
  const uint32_t note_id = checkedUintFromVal(note_id_value, "noteId");
  PitchGuard result;
  check(
      [&] {
        return draft ? sonare_vocal_draft_evaluate_pitch(
                           handlePointer<const SonareVocalEditDraft>(handle, "draft"), note_id,
                           &result.value)
                     : sonare_vocal_session_evaluate_pitch(
                           handlePointer<const SonareVocalEditSession>(handle, "session"), note_id,
                           &result.value);
      },
      "vocal evaluate pitch");
  return pitchResultToVal(result.value);
}

double js_vocal_edit_map_coordinate(double handle, const val& note_id_value,
                                    const val& sample_value, bool draft, bool inverse) {
  const uint32_t note_id = checkedUintFromVal(note_id_value, "noteId");
  const double sample = checkedDoubleFromVal(sample_value, "sample");
  double output = 0.0;
  check(
      [&] {
        if (draft && inverse)
          return sonare_vocal_draft_destination_to_source(
              handlePointer<const SonareVocalEditDraft>(handle, "draft"), note_id, sample, &output);
        if (draft)
          return sonare_vocal_draft_source_to_destination(
              handlePointer<const SonareVocalEditDraft>(handle, "draft"), note_id, sample, &output);
        if (inverse)
          return sonare_vocal_session_destination_to_source(
              handlePointer<const SonareVocalEditSession>(handle, "session"), note_id, sample,
              &output);
        return sonare_vocal_session_source_to_destination(
            handlePointer<const SonareVocalEditSession>(handle, "session"), note_id, sample,
            &output);
      },
      "vocal coordinate mapping");
  return output;
}

double js_vocal_edit_capture_snapshot(double handle, bool draft) {
  SonareVocalRenderSnapshot* snapshot = nullptr;
  check(
      [&] {
        return draft
                   ? sonare_vocal_draft_capture_snapshot(
                         handlePointer<const SonareVocalEditDraft>(handle, "draft"), &snapshot)
                   : sonare_vocal_session_capture_snapshot(
                         handlePointer<const SonareVocalEditSession>(handle, "session"), &snapshot);
      },
      "vocal capture snapshot");
  return handleValue(snapshot);
}

void js_vocal_edit_snapshot_destroy(double handle) {
  if (handle != 0.0)
    sonare_vocal_snapshot_destroy(handlePointer<SonareVocalRenderSnapshot>(handle, "snapshot"));
}

double js_vocal_edit_snapshot_output_length(double handle) {
  int64_t samples = 0;
  check(
      [&] {
        return sonare_vocal_snapshot_output_length(
            handlePointer<const SonareVocalRenderSnapshot>(handle, "snapshot"), &samples);
      },
      "vocal snapshot output length");
  return static_cast<double>(samples);
}

val js_vocal_edit_snapshot_render(double handle, const val& start_value, const val& end_value,
                                  const val& request_id_value) {
  const int64_t start = checkedInt64FromVal(start_value, "startSample");
  const int64_t end = checkedInt64FromVal(end_value, "endSample");
  RenderGuard result;
  const SonareVocalRange range{start, end};
  check(
      [&] {
        return sonare_vocal_snapshot_render(
            handlePointer<const SonareVocalRenderSnapshot>(handle, "snapshot"), range,
            checkedDecimalUint64FromVal(request_id_value, "requestId"), nullptr, nullptr,
            &result.value);
      },
      "vocal snapshot render");
  return renderResultToVal(result.value);
}

double js_vocal_edit_render_job_begin(double handle, const val& start_value, const val& end_value,
                                      const val& request_id_value) {
  const int64_t start = checkedInt64FromVal(start_value, "startSample");
  const int64_t end = checkedInt64FromVal(end_value, "endSample");
  SonareVocalRenderJob* job = nullptr;
  check(
      [&] {
        return sonare_vocal_render_job_begin(
            handlePointer<const SonareVocalRenderSnapshot>(handle, "snapshot"), {start, end},
            checkedDecimalUint64FromVal(request_id_value, "requestId"), &job);
      },
      "vocal render job begin");
  return handleValue(job);
}

bool js_vocal_edit_render_job_next(double handle) {
  int complete = 0;
  check(
      [&] {
        return sonare_vocal_render_job_next(handlePointer<SonareVocalRenderJob>(handle, "job"),
                                            nullptr, nullptr, &complete);
      },
      "vocal render job next");
  return complete != 0;
}

val js_vocal_edit_render_job_finalize(double handle) {
  RenderGuard result;
  check(
      [&] {
        return sonare_vocal_render_job_finalize(handlePointer<SonareVocalRenderJob>(handle, "job"),
                                                nullptr, nullptr, &result.value);
      },
      "vocal render job finalize");
  return renderResultToVal(result.value);
}

void js_vocal_edit_render_job_abort(double handle) {
  if (handle != 0.0)
    sonare_vocal_render_job_abort(handlePointer<SonareVocalRenderJob>(handle, "job"));
}

void js_vocal_edit_render_job_destroy(double handle) {
  if (handle != 0.0)
    sonare_vocal_render_job_destroy(handlePointer<SonareVocalRenderJob>(handle, "job"));
}

val js_vocal_edit_session_export_state(double handle) {
  StateGuard result;
  check(
      [&] {
        return sonare_vocal_session_export_state(
            handlePointer<const SonareVocalEditSession>(handle, "session"), &result.value);
      },
      "vocal export state");
  return vectorToUint8Array(result.value.data, static_cast<std::size_t>(result.value.size));
}

val js_vocal_edit_last_error_detail() {
  SonareVocalErrorDetail detail{};
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  const auto textOrNumber = [](const char* text, uint64_t number) {
    return text != nullptr && text[0] != '\0' ? std::string(text) : std::to_string(number);
  };
  val result = val::object();
  result.set("sequence", std::to_string(vocal_error_sequence));
  result.set("reason", detail.reason);
  result.set("field", std::string(detail.field));
  result.set("expected", textOrNumber(detail.expected_text, detail.expected));
  result.set("actual", textOrNumber(detail.actual_text, detail.actual));
  result.set("expectedText", std::string(detail.expected_text));
  result.set("actualText", std::string(detail.actual_text));
  return result;
}

uint32_t js_vocal_edit_api_version() { return sonare_vocal_edit_api_version(); }

int js_vocal_edit_available() { return sonare_vocal_available(); }

void registerVocalEditBindingsImpl() {
  function("vocalEditApiVersion", &js_vocal_edit_api_version);
  function("vocalEditAvailable", &js_vocal_edit_available);
  function("vocalEditSessionCreate", &js_vocal_edit_session_create);
  function("vocalEditSessionRestore", &js_vocal_edit_session_restore);
  function("vocalEditSessionDestroy", &js_vocal_edit_session_destroy);
  function("vocalEditSessionNotes", &js_vocal_edit_session_notes);
  function("vocalEditDraftNotes", &js_vocal_edit_draft_notes);
  function("vocalEditSessionToken", &js_vocal_edit_session_token);
  function("vocalEditDraftToken", &js_vocal_edit_draft_token);
  function("vocalEditSessionAnalysis", &js_vocal_edit_session_analysis);
  function("vocalEditSessionCapabilities", &js_vocal_edit_session_capabilities);
  function("vocalEditSessionOutputLength", &js_vocal_edit_session_output_length);
  function("vocalEditSessionHistory", &js_vocal_edit_session_history);
  function("vocalEditSessionBeginEdit", &js_vocal_edit_session_begin_edit);
  function("vocalEditDraftApply", &js_vocal_edit_draft_apply);
  function("vocalEditDraftCommit", &js_vocal_edit_draft_commit);
  function("vocalEditDraftCancel", &js_vocal_edit_draft_cancel);
  function("vocalEditDraftDestroy", &js_vocal_edit_draft_destroy);
  function("vocalEditSessionApplyHistory", &js_vocal_edit_session_apply_history);
  function("vocalEditEvaluatePitch", &js_vocal_edit_evaluate_pitch);
  function("vocalEditMapCoordinate", &js_vocal_edit_map_coordinate);
  function("vocalEditCaptureSnapshot", &js_vocal_edit_capture_snapshot);
  function("vocalEditSnapshotDestroy", &js_vocal_edit_snapshot_destroy);
  function("vocalEditSnapshotOutputLength", &js_vocal_edit_snapshot_output_length);
  function("vocalEditSnapshotRender", &js_vocal_edit_snapshot_render);
  function("vocalEditRenderJobBegin", &js_vocal_edit_render_job_begin);
  function("vocalEditRenderJobNext", &js_vocal_edit_render_job_next);
  function("vocalEditRenderJobFinalize", &js_vocal_edit_render_job_finalize);
  function("vocalEditRenderJobAbort", &js_vocal_edit_render_job_abort);
  function("vocalEditRenderJobDestroy", &js_vocal_edit_render_job_destroy);
  function("vocalEditSessionExportState", &js_vocal_edit_session_export_state);
  function("vocalEditLastErrorDetail", &js_vocal_edit_last_error_detail);
}

}  // namespace

void registerVocalEditBindings() { registerVocalEditBindingsImpl(); }

#endif  // __EMSCRIPTEN__
