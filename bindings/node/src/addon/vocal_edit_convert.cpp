#include "vocal_edit_convert.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "c_api/editing_input_domains.h"
#include "core/audio.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::vocal_edit_detail {

bool ReadArrayNumber(Napi::Env env, const Napi::Value& value, const char* field,
                     std::vector<float>* out) {
  out->clear();
  if (value.IsUndefined() || value.IsNull()) return true;
  if (IsFloat32Array(value)) {
    const auto array = value.As<Napi::Float32Array>();
    out->assign(sonare_node::ElementData(array),
                sonare_node::ElementData(array) + array.ElementLength());
  } else if (value.IsArray()) {
    const auto array = value.As<Napi::Array>();
    const uint32_t array_count = node_list_length(array, field);
    out->reserve(array_count);
    for (uint32_t i = 0; i < array_count; ++i) {
      double number = 0.0;
      const std::string name = std::string(field) + "[" + std::to_string(i) + "]";
      if (!RequiredFiniteDoubleValue(env, array.Get(i), name.c_str(), &number, 0.0,
                                     static_cast<double>(std::numeric_limits<float>::max())))
        return false;
      out->push_back(static_cast<float>(number));
    }
  } else {
    Napi::TypeError::New(env, std::string(field) + " must be a Float32Array or number[]")
        .ThrowAsJavaScriptException();
    return false;
  }
  for (size_t i = 0; i < out->size(); ++i) {
    if (!std::isfinite((*out)[i]) || (*out)[i] < 0.0f) {
      Napi::RangeError::New(env, std::string(field) + " must contain finite values of at least 0")
          .ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

bool ReadStringEnum(Napi::Env env, const Napi::Value& value, const char* field,
                    const char* const* names, size_t count, uint32_t* out) {
  const std::string name = node_narrow_string(env, value, field);
  for (size_t i = 0; i < count; ++i) {
    if (name == names[i]) {
      *out = static_cast<uint32_t>(i);
      return true;
    }
  }
  Napi::RangeError::New(env, std::string(field) + " has an unsupported value")
      .ThrowAsJavaScriptException();
  return false;
}

bool ReadRange(Napi::Env env, const Napi::Value& value, SonareVocalRange* out) {
  Napi::Object object;
  if (!RequiredObjectValue(env, value, "range", &object)) return false;
  int64_t start = 0;
  int64_t end = 0;
  if (!RequiredSafeIntegerValue(env, object.Get("startSample"), "range.startSample", &start, 0) ||
      !RequiredSafeIntegerValue(env, object.Get("endSample"), "range.endSample", &end, 0)) {
    return false;
  }
  out->start_sample = start;
  out->end_sample = end;
  return true;
}

bool ReadNoteId(Napi::Env env, const Napi::Value& value, const char* field, uint32_t* out) {
  if (!RequiredUint32Value(env, value, field, out)) return false;
  if (*out == 0) {
    Napi::RangeError::New(env, std::string(field) + " must be non-zero")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

bool ReadVocalNoteEdit(Napi::Env env, const Napi::Value& value, SonareVocalNoteEdit* out,
                       EditStorage* storage) {
  Napi::Object object;
  if (!RequiredObjectValue(env, value, "edit", &object)) return false;
  sonare_vocal_note_edit_init(out);

  const Napi::Value pitch_value = object.Get("pitch");
  Napi::Object pitch;
  if (!RequiredObjectValue(env, pitch_value, "edit.pitch", &pitch)) return false;
  const Napi::Value target_value = pitch.Get("target");
  Napi::Object target;
  if (!RequiredObjectValue(env, target_value, "edit.pitch.target", &target)) return false;
  uint32_t target_mode = 0;
  const char* const target_names[] = {"none", "center", "curve"};
  if (!ReadStringEnum(env, target.Get("mode"), "edit.pitch.target.mode", target_names,
                      std::size(target_names), &target_mode)) {
    return false;
  }
  out->target_mode = target_mode;
  if (target_mode == SONARE_VOCAL_TARGET_CENTER) {
    if (!RequiredDoubleProperty(env, target, "midi", &out->target_midi)) return false;
    if (!std::isfinite(out->target_midi)) {
      Napi::RangeError::New(env, "midi must be finite").ThrowAsJavaScriptException();
      return false;
    }
    if (out->target_midi < 0.0 || out->target_midi > 127.0) {
      Napi::RangeError::New(env, "edit.pitch.target.midi must be within [0, 127]")
          .ThrowAsJavaScriptException();
      return false;
    }
  } else if (target_mode == SONARE_VOCAL_TARGET_CURVE) {
    const Napi::Value points_value = target.Get("points");
    if (!points_value.IsArray()) {
      Napi::TypeError::New(env, "edit.pitch.target.points must be an array")
          .ThrowAsJavaScriptException();
      return false;
    }
    const Napi::Array points = points_value.As<Napi::Array>();
    storage->points.clear();
    const uint32_t point_count = node_list_length(points, "edit.pitch.target.points");
    storage->points.reserve(point_count);
    double previous = -std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < point_count; ++i) {
      Napi::Object point;
      std::string at;
      double source_sample = 0.0;
      double midi = 0.0;
      if (!RequiredElementObject(env, points, i, "edit.pitch.target.points", &point, &at) ||
          !RequiredDoubleValue(env, point.Get("sourceSample"), at + ".sourceSample",
                               &source_sample) ||
          !RequiredDoubleValue(env, point.Get("midi"), at + ".midi", &midi))
        return false;
      if (!std::isfinite(source_sample)) {
        Napi::RangeError::New(env, "sourceSample must be finite").ThrowAsJavaScriptException();
        return false;
      }
      if (!std::isfinite(midi)) {
        Napi::RangeError::New(env, "midi must be finite").ThrowAsJavaScriptException();
        return false;
      }
      if (source_sample <= previous || midi < 0.0 || midi > 127.0) {
        Napi::RangeError::New(
            env, "curve points must be strictly increasing and MIDI must be within [0, 127]")
            .ThrowAsJavaScriptException();
        return false;
      }
      previous = source_sample;
      storage->points.push_back({source_sample, midi});
    }
    if (storage->points.size() < 2) {
      Napi::RangeError::New(env, "edit.pitch.target.points must contain at least two points")
          .ThrowAsJavaScriptException();
      return false;
    }
    out->target_points = sonare_node::ElementData(storage->points);
    out->target_point_count = storage->points.size();
  }

  const auto pitch_number = [&](const char* key, double* destination, double minimum,
                                double maximum) {
    const Napi::Value property = pitch.Get(key);
    if (property.IsUndefined() || property.IsNull()) return true;
    return RequiredFiniteDoubleValue(env, property, key, destination, minimum, maximum);
  };
  if (!pitch_number("amount", &out->amount, 0.0, 1.0) ||
      !pitch_number("speedMs", &out->speed_ms, 0.0, std::numeric_limits<double>::infinity()) ||
      !pitch_number("maxCorrectionSemitones", &out->max_correction_semitones, 0.0,
                    std::numeric_limits<double>::infinity()) ||
      !pitch_number("transposeSemitones", &out->transpose_semitones,
                    -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()) ||
      !pitch_number("driftScale", &out->drift_scale, 0.0,
                    std::numeric_limits<double>::infinity()) ||
      !pitch_number("vibratoScale", &out->vibrato_scale, 0.0,
                    std::numeric_limits<double>::infinity())) {
    return false;
  }

  // The destination placement is required: an omitted position is never read as 0.
  if (!RequiredSafeIntegerValue(env, object.Get("destinationStartSample"),
                                "edit.destinationStartSample", &out->destination_start_sample, 0) ||
      !RequiredSafeIntegerValue(env, object.Get("destinationLengthSamples"),
                                "edit.destinationLengthSamples", &out->destination_length_samples,
                                0)) {
    return false;
  }
  const Napi::Value gain = object.Get("gainDb");
  if (!gain.IsUndefined() && !gain.IsNull() &&
      !RequiredFiniteDoubleValue(env, gain, "edit.gainDb", &out->gain_db))
    return false;
  const Napi::Value muted = object.Get("muted");
  if (!muted.IsUndefined() && !muted.IsNull()) {
    bool flag = false;
    if (!RequiredBoolValue(env, muted, "edit.muted", &flag)) return false;
    out->muted = flag ? 1u : 0u;
  }

  Napi::Object formant;
  const Napi::Value formant_value = object.Get("formant");
  if (!formant_value.IsUndefined() && !formant_value.IsNull()) {
    if (!RequiredObjectValue(env, formant_value, "edit.formant", &formant)) return false;
    const char* const formant_names[] = {"preserve", "shift"};
    if (!ReadStringEnum(env, formant.Get("mode"), "edit.formant.mode", formant_names,
                        std::size(formant_names), &out->formant_mode)) {
      return false;
    }
    const Napi::Value shift = formant.Get("shiftSemitones");
    if (!shift.IsUndefined() && !shift.IsNull() &&
        !RequiredFiniteDoubleValue(env, shift, "edit.formant.shiftSemitones",
                                   &out->formant_shift_semitones)) {
      return false;
    }
  }
  if (!ReadArrayNumber(env, object.Get("amplitudeEnvelope"), "edit.amplitudeEnvelope",
                       &storage->envelope)) {
    return false;
  }
  out->amplitude_envelope = storage->envelope.empty() ? nullptr : storage->envelope.data();
  out->amplitude_envelope_count = storage->envelope.size();
  return true;
}

bool ReadTransition(Napi::Env env, const Napi::Value& value, SonareVocalTransition* out) {
  Napi::Object object;
  if (!RequiredObjectValue(env, value, "transition", &object)) return false;
  *out = {};
  out->struct_size = sizeof(*out);
  const Napi::Value left = object.Get("leftNoteId");
  const Napi::Value right = object.Get("rightNoteId");
  if (!ReadNoteId(env, left, "transition.leftNoteId", &out->left_note_id) ||
      !ReadNoteId(env, right, "transition.rightNoteId", &out->right_note_id))
    return false;
  if (!RequiredSafeIntegerValue(env, object.Get("leftWindowSamples"),
                                "transition.leftWindowSamples", &out->left_window_samples, 0) ||
      !RequiredSafeIntegerValue(env, object.Get("rightWindowSamples"),
                                "transition.rightWindowSamples", &out->right_window_samples, 0) ||
      !RequiredFiniteDoubleValue(env, object.Get("strength"), "transition.strength", &out->strength,
                                 0.0, 1.0)) {
    return false;
  }
  const Napi::Value curve = object.Get("curve");
  if (curve.IsUndefined() || curve.IsNull() || !curve.IsString() ||
      node_narrow_string(env, curve, "transition.curve") != "smoothstep") {
    Napi::RangeError::New(env, "transition.curve must be smoothstep").ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

bool ReadOperations(Napi::Env env, const Napi::Value& value, std::vector<ParsedOperation>* parsed) {
  if (!value.IsArray()) {
    Napi::TypeError::New(env, "operations must be an array").ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Array array = value.As<Napi::Array>();
  parsed->clear();
  const uint32_t array_count = node_list_length(array, "operations");
  parsed->reserve(array_count);
  for (uint32_t i = 0; i < array_count; ++i) {
    Napi::Object object;
    if (!RequiredObjectValue(env, array.Get(i), "operations[]", &object)) return false;
    const Napi::Value kind_value = object.Get("kind");
    if (!kind_value.IsString()) {
      Napi::TypeError::New(env, "operation.kind must be a string").ThrowAsJavaScriptException();
      return false;
    }
    ParsedOperation item;
    sonare_vocal_operation_init(&item.operation);
    const std::string kind = node_narrow_string(env, kind_value, "operation.kind");
    if (kind == "setEdit") {
      item.operation.kind = SONARE_VOCAL_SET_EDIT;
      if (!ReadNoteId(env, object.Get("noteId"), "operation.noteId", &item.operation.note_id) ||
          !ReadVocalNoteEdit(env, object.Get("edit"), &item.operation.edit, &item.edit_storage)) {
        return false;
      }
    } else if (kind == "setSourceSpan") {
      item.operation.kind = SONARE_VOCAL_SET_SOURCE_SPAN;
      if (!ReadNoteId(env, object.Get("noteId"), "operation.noteId", &item.operation.note_id) ||
          !RequiredSafeIntegerValue(env, object.Get("sourceStartSample"),
                                    "operation.sourceStartSample",
                                    &item.operation.source_start_sample, 0) ||
          !RequiredSafeIntegerValue(env, object.Get("sourceEndSample"), "operation.sourceEndSample",
                                    &item.operation.source_end_sample, 0) ||
          !RequiredSafeIntegerValue(env, object.Get("destinationStartSample"),
                                    "operation.destinationStartSample",
                                    &item.operation.destination_start_sample, 0) ||
          !RequiredSafeIntegerValue(env, object.Get("destinationLengthSamples"),
                                    "operation.destinationLengthSamples",
                                    &item.operation.destination_length_samples, 0)) {
        return false;
      }
    } else if (kind == "split") {
      item.operation.kind = SONARE_VOCAL_SPLIT;
      if (!ReadNoteId(env, object.Get("noteId"), "operation.noteId", &item.operation.note_id) ||
          !RequiredSafeIntegerValue(env, object.Get("sourceSample"), "operation.sourceSample",
                                    &item.operation.source_sample, 0)) {
        return false;
      }
    } else if (kind == "merge") {
      item.operation.kind = SONARE_VOCAL_MERGE;
      const Napi::Value ids_value = object.Get("noteIds");
      if (!ids_value.IsArray()) {
        Napi::TypeError::New(env, "operation.noteIds must be an array")
            .ThrowAsJavaScriptException();
        return false;
      }
      const Napi::Array ids = ids_value.As<Napi::Array>();
      const uint32_t id_count = node_list_length(ids, "operation.noteIds");
      item.note_ids.reserve(id_count);
      for (uint32_t j = 0; j < id_count; ++j) {
        uint32_t id = 0;
        if (!ReadNoteId(env, ids.Get(j), "operation.noteIds[]", &id)) return false;
        item.note_ids.push_back(id);
      }
      item.operation.note_ids = sonare_node::ElementData(item.note_ids);
      item.operation.note_id_count = item.note_ids.size();
      const Napi::Value policy = object.Get("policy");
      if (policy.IsUndefined() || policy.IsNull()) {
        item.operation.policy = SONARE_VOCAL_MERGE_PRESERVE;
      } else {
        const char* const policy_names[] = {"preserve", "reset"};
        if (!ReadStringEnum(env, policy, "operation.policy", policy_names, std::size(policy_names),
                            &item.operation.policy))
          return false;
      }
    } else if (kind == "setTransition") {
      item.operation.kind = SONARE_VOCAL_SET_TRANSITION;
      if (!ReadTransition(env, object.Get("transition"), &item.operation.transition)) return false;
    } else if (kind == "removeTransition") {
      item.operation.kind = SONARE_VOCAL_REMOVE_TRANSITION;
      if (!ReadNoteId(env, object.Get("leftNoteId"), "operation.leftNoteId",
                      &item.operation.transition.left_note_id) ||
          !ReadNoteId(env, object.Get("rightNoteId"), "operation.rightNoteId",
                      &item.operation.transition.right_note_id)) {
        return false;
      }
    } else if (kind == "reset") {
      item.operation.kind = SONARE_VOCAL_RESET;
      const Napi::Value ids_value = object.Get("noteIds");
      if (!ids_value.IsArray()) {
        Napi::TypeError::New(env, "operation.noteIds must be an array")
            .ThrowAsJavaScriptException();
        return false;
      }
      const Napi::Array ids = ids_value.As<Napi::Array>();
      const uint32_t id_count = node_list_length(ids, "operation.noteIds");
      item.note_ids.reserve(id_count);
      for (uint32_t j = 0; j < id_count; ++j) {
        uint32_t id = 0;
        if (!ReadNoteId(env, ids.Get(j), "operation.noteIds[]", &id)) return false;
        item.note_ids.push_back(id);
      }
      item.operation.note_ids = sonare_node::ElementData(item.note_ids);
      item.operation.note_id_count = item.note_ids.size();
    } else {
      Napi::RangeError::New(env, "operation.kind has an unsupported value")
          .ThrowAsJavaScriptException();
      return false;
    }
    parsed->push_back(std::move(item));
  }
  return true;
}

std::string Uint64String(uint64_t value) { return std::to_string(value); }

Napi::Object TokenToObject(Napi::Env env, const SonareVocalStateToken& token) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("sessionEpoch", Uint64String(token.session_epoch));
  object.Set("revision", Uint64String(token.revision));
  object.Set("draftId", Uint64String(token.draft_id));
  object.Set("generation", Uint64String(token.generation));
  object.Set("requestId", Uint64String(token.request_id));
  object.Set("profileId", Napi::Number::New(env, token.profile_id));
  return object;
}

Napi::Object RangeToObject(Napi::Env env, const SonareVocalRange& range) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("startSample", Napi::Number::New(env, static_cast<double>(range.start_sample)));
  object.Set("endSample", Napi::Number::New(env, static_cast<double>(range.end_sample)));
  return object;
}

Napi::Object PointToObject(Napi::Env env, const SonareVocalPitchPoint& point) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("sourceSample", Napi::Number::New(env, point.source_sample));
  object.Set("midi", Napi::Number::New(env, point.midi));
  return object;
}

namespace {

/// The spelling of an enum ordinal the library returned; one past the table is a mirror that has
/// drifted from the core, refused as an InvalidFormat rather than clamped onto the last member.
const char* LibraryEnumName(Napi::Env env, const char* const* names, size_t count, uint32_t ordinal,
                            const char* noun) {
  if (ordinal >= count) {
    throw sonare_node::MakeSonareError(
        env, SONARE_ERROR_INVALID_FORMAT,
        std::string("unknown ") + noun + " ordinal: " + std::to_string(ordinal));
  }
  return names[ordinal];
}

}  // namespace

Napi::Object EditToObject(Napi::Env env, const SonareVocalNoteEdit& edit) {
  Napi::Object pitch = Napi::Object::New(env);
  Napi::Object target = Napi::Object::New(env);
  const char* const target_names[] = {"none", "center", "curve"};
  target.Set("mode", LibraryEnumName(env, target_names, std::size(target_names), edit.target_mode,
                                     "vocal target mode"));
  if (edit.target_mode == SONARE_VOCAL_TARGET_CENTER) {
    target.Set("midi", edit.target_midi);
  } else if (edit.target_mode == SONARE_VOCAL_TARGET_CURVE) {
    Napi::Array points = Napi::Array::New(env, edit.target_point_count);
    for (uint64_t i = 0; i < edit.target_point_count; ++i) {
      points.Set(static_cast<uint32_t>(i), PointToObject(env, edit.target_points[i]));
    }
    target.Set("points", points);
  }
  pitch.Set("target", target);
  pitch.Set("amount", edit.amount);
  pitch.Set("speedMs", edit.speed_ms);
  pitch.Set("maxCorrectionSemitones", edit.max_correction_semitones);
  pitch.Set("transposeSemitones", edit.transpose_semitones);
  pitch.Set("driftScale", edit.drift_scale);
  pitch.Set("vibratoScale", edit.vibrato_scale);

  Napi::Object formant = Napi::Object::New(env);
  const char* const formant_names[] = {"preserve", "shift"};
  formant.Set("mode", LibraryEnumName(env, formant_names, std::size(formant_names),
                                      edit.formant_mode, "vocal formant mode"));
  formant.Set("shiftSemitones", edit.formant_shift_semitones);
  Napi::Object result = Napi::Object::New(env);
  result.Set("pitch", pitch);
  result.Set("destinationStartSample",
             Napi::Number::New(env, static_cast<double>(edit.destination_start_sample)));
  result.Set("destinationLengthSamples",
             Napi::Number::New(env, static_cast<double>(edit.destination_length_samples)));
  result.Set("gainDb", edit.gain_db);
  result.Set("muted", Napi::Boolean::New(env, edit.muted != 0));
  Napi::Array envelope = Napi::Array::New(env, edit.amplitude_envelope_count);
  for (uint64_t i = 0; i < edit.amplitude_envelope_count; ++i) {
    envelope.Set(static_cast<uint32_t>(i),
                 Napi::Number::New(env, static_cast<double>(edit.amplitude_envelope[i])));
  }
  result.Set("amplitudeEnvelope", envelope);
  result.Set("formant", formant);
  return result;
}

Napi::Object NoteToObject(Napi::Env env, const SonareVocalNote& note) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("id", Napi::Number::New(env, note.id));
  result.Set("sourceStartSample",
             Napi::Number::New(env, static_cast<double>(note.source_start_sample)));
  result.Set("sourceEndSample",
             Napi::Number::New(env, static_cast<double>(note.source_end_sample)));
  result.Set("analysisFrameStart", Uint64String(note.analysis_frame_start));
  result.Set("analysisFrameEnd", Uint64String(note.analysis_frame_end));
  result.Set("hasPitch", Napi::Boolean::New(env, note.has_pitch != 0));
  result.Set("medianHz", note.median_hz);
  result.Set("centerMidi", note.center_midi);
  result.Set("f0Stability", note.f0_stability);
  Napi::Float32Array amplitude = Float32ArrayFrom(env, note.amplitude, note.amplitude_count);
  result.Set("amplitude", amplitude);
  result.Set("edit", EditToObject(env, note.edit));
  return result;
}

Napi::Object TransitionToObject(Napi::Env env, const SonareVocalTransition& transition) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("leftNoteId", Napi::Number::New(env, transition.left_note_id));
  result.Set("rightNoteId", Napi::Number::New(env, transition.right_note_id));
  result.Set("leftWindowSamples",
             Napi::Number::New(env, static_cast<double>(transition.left_window_samples)));
  result.Set("rightWindowSamples",
             Napi::Number::New(env, static_cast<double>(transition.right_window_samples)));
  result.Set("strength", transition.strength);
  result.Set("curve", "smoothstep");
  return result;
}

Napi::Object NotesResultToObject(Napi::Env env, const SonareVocalNotesResult& result) {
  Napi::Object object = Napi::Object::New(env);
  Napi::Array notes = Napi::Array::New(env, result.note_count);
  for (uint64_t i = 0; i < result.note_count; ++i) {
    notes.Set(static_cast<uint32_t>(i), NoteToObject(env, result.notes[i]));
  }
  Napi::Array transitions = Napi::Array::New(env, result.transition_count);
  for (uint64_t i = 0; i < result.transition_count; ++i) {
    transitions.Set(static_cast<uint32_t>(i), TransitionToObject(env, result.transitions[i]));
  }
  object.Set("notes", notes);
  object.Set("transitions", transitions);
  return object;
}

Napi::Object AnalysisResultToObject(Napi::Env env, const SonareVocalAnalysisResult& result) {
  Napi::Object analysis = Napi::Object::New(env);
  analysis.Set("frameOriginSample", result.analysis.frame_origin_sample);
  analysis.Set("samplesPerFrame", result.analysis.samples_per_frame);
  analysis.Set("frameLengthSamples", result.analysis.frame_length_samples);
  Napi::Float32Array f0 = Napi::Float32Array::New(env, result.analysis.frame_count);
  Napi::Uint8Array voiced = Napi::Uint8Array::New(env, result.analysis.frame_count);
  if (result.analysis.frame_count != 0) {
    std::memcpy(sonare_node::ElementData(f0), result.analysis.f0_hz,
                result.analysis.frame_count * sizeof(float));
    std::memcpy(sonare_node::ElementData(voiced), result.analysis.voiced,
                result.analysis.frame_count);
  }
  analysis.Set("f0Hz", f0);
  analysis.Set("voiced", voiced);
  analysis.Set("algorithmId",
               result.analysis.algorithm_id == nullptr ? "" : result.analysis.algorithm_id);
  analysis.Set("algorithmVersion", result.analysis.algorithm_version);
  analysis.Set("fminHz", result.analysis.fmin_hz);
  analysis.Set("fmaxHz", result.analysis.fmax_hz);
  analysis.Set("yinThreshold", result.analysis.yin_threshold);
  analysis.Set("voicedThreshold", result.analysis.voiced_threshold);
  analysis.Set("centered", Napi::Boolean::New(env, result.analysis.centered != 0));
  analysis.Set("segmentationThresholdCents", result.analysis.segmentation_threshold_cents);
  analysis.Set("minNoteMs", result.analysis.min_note_ms);
  analysis.Set("referenceHz", result.analysis.reference_hz);
  analysis.Set("sourceLengthSamples",
               Napi::Number::New(env, static_cast<double>(result.source_length_samples)));
  analysis.Set("sampleRate", result.sample_rate);
  analysis.Set("sourceSha256", result.source_sha256);
  analysis.Set("analysisSha256", result.analysis_sha256);
  return analysis;
}

Napi::Object CapabilitiesToObject(Napi::Env env, const SonareVocalCapabilities& capabilities) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("abiVersion", capabilities.abi_version);
  object.Set("profileId", capabilities.profile_id);
  object.Set("monophonicOnly", Napi::Boolean::New(env, capabilities.monophonic_only != 0));
  object.Set("analysisCancellable",
             Napi::Boolean::New(env, capabilities.analysis_cancellable != 0));
  object.Set("minimumFormantShiftSemitones", capabilities.minimum_formant_shift_semitones);
  object.Set("maximumFormantShiftSemitones", capabilities.maximum_formant_shift_semitones);
  object.Set("vocalEdit", true);
  return object;
}

Napi::Object EditResultToObject(Napi::Env env, const SonareVocalEditResult& result) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("token", TokenToObject(env, result.token));
  Napi::Array dirty = Napi::Array::New(env, result.dirty_range_count);
  for (uint64_t i = 0; i < result.dirty_range_count; ++i) {
    dirty.Set(static_cast<uint32_t>(i), RangeToObject(env, result.dirty_ranges[i]));
  }
  Napi::Array changes = Napi::Array::New(env, result.id_change_count);
  for (uint64_t i = 0; i < result.id_change_count; ++i) {
    const SonareVocalIdChange& change = result.id_changes[i];
    Napi::Object item = Napi::Object::New(env);
    item.Set("operationIndex", change.operation_index);
    item.Set("retiredId", change.retired_id);
    Napi::Array ids = Napi::Array::New(env, change.second_new_id == 0 ? 1 : 2);
    ids.Set(uint32_t{0}, change.first_new_id);
    if (change.second_new_id != 0) ids.Set(uint32_t{1}, change.second_new_id);
    item.Set("newIds", ids);
    changes.Set(static_cast<uint32_t>(i), item);
  }
  object.Set("dirtyRanges", dirty);
  object.Set("idChanges", changes);
  return object;
}

Napi::Object PitchResultToObject(Napi::Env env, const SonareVocalPitchResult& result) {
  Napi::Object object = Napi::Object::New(env);
  Napi::Float64Array source = Napi::Float64Array::New(env, result.frame_count);
  Napi::Float64Array measured = Napi::Float64Array::New(env, result.frame_count);
  Napi::Float64Array target = Napi::Float64Array::New(env, result.frame_count);
  Napi::Float64Array effective = Napi::Float64Array::New(env, result.frame_count);
  Napi::Uint8Array voiced = Napi::Uint8Array::New(env, result.frame_count);
  Napi::Uint8Array has_target = Napi::Uint8Array::New(env, result.frame_count);
  if (result.frame_count != 0) {
    std::memcpy(sonare_node::ElementData(source), result.source_samples,
                result.frame_count * sizeof(double));
    std::memcpy(sonare_node::ElementData(measured), result.measured_midi,
                result.frame_count * sizeof(double));
    std::memcpy(sonare_node::ElementData(target), result.target_midi,
                result.frame_count * sizeof(double));
    std::memcpy(sonare_node::ElementData(effective), result.effective_midi,
                result.frame_count * sizeof(double));
    std::memcpy(sonare_node::ElementData(voiced), result.voiced, result.frame_count);
    std::memcpy(sonare_node::ElementData(has_target), result.has_target, result.frame_count);
  }
  object.Set("sourceSamples", source);
  object.Set("measuredMidi", measured);
  object.Set("targetMidi", target);
  object.Set("effectiveMidi", effective);
  object.Set("voiced", voiced);
  object.Set("hasTarget", has_target);
  return object;
}

Napi::Object RenderResultToObject(Napi::Env env, const SonareVocalRenderResult& result) {
  Napi::Object object = Napi::Object::New(env);
  Napi::Float32Array samples =
      Float32ArrayFrom(env, result.samples, static_cast<size_t>(result.sample_count));
  object.Set("samples", samples);
  object.Set("startSample", Napi::Number::New(env, static_cast<double>(result.start_sample)));
  object.Set("token", TokenToObject(env, result.token));
  Napi::Array ranges = Napi::Array::New(env, result.processed_range_count);
  for (uint64_t i = 0; i < result.processed_range_count; ++i) {
    ranges.Set(static_cast<uint32_t>(i), RangeToObject(env, result.processed_ranges[i]));
  }
  object.Set("processedRanges", ranges);
  object.Set("cacheHitUnits", Uint64String(result.cache_hit_units));
  object.Set("dryPassedFrames", Uint64String(result.dry_passed_frames));
  object.Set("limitedCorrectionFrames", Uint64String(result.limited_correction_frames));
  return object;
}

Napi::Error MakeVocalError(Napi::Env env, SonareError error,
                           const SonareVocalErrorDetail* detail = nullptr) {
  Napi::Error js_error = sonare_node::MakeSonareError(
      env, error, sonare_node::ErrorMessageForCode(error), sonare_node::LastRefusalIsShape());
  Napi::Object object = js_error.Value();
  if (const auto shown = sonare_node::ShownLastRefusal()) {
    sonare_node::AttachRefusal(env, object, *shown, sonare_node::LastRefusalEntry());
  }
  object.Set("reason", detail == nullptr ? 0u : detail->reason);
  object.Set("field", detail == nullptr ? "" : detail->field);
  object.Set("expected", detail == nullptr
                             ? "0"
                             : (detail->expected_text[0] == '\0' ? Uint64String(detail->expected)
                                                                 : detail->expected_text));
  object.Set("actual", detail == nullptr
                           ? "0"
                           : (detail->actual_text[0] == '\0' ? Uint64String(detail->actual)
                                                             : detail->actual_text));
  object.Set("expectedText", detail == nullptr ? "" : detail->expected_text);
  object.Set("actualText", detail == nullptr ? "" : detail->actual_text);
  return js_error;
}

bool CheckVocalError(Napi::Env env, SonareError error) {
  if (error == SONARE_OK) return true;
  SonareVocalErrorDetail detail{};
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  Napi::Error js_error = MakeVocalError(env, error, &detail);
  js_error.ThrowAsJavaScriptException();
  return false;
}

Napi::Promise RejectedVocalPromise(Napi::Env env, SonareError error) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  SonareVocalErrorDetail detail{};
  sonare_vocal_error_detail_init(&detail);
  sonare_vocal_last_error_detail(&detail);
  deferred.Reject(MakeVocalError(env, error, &detail).Value());
  return deferred.Promise();
}

bool ReadSampleRate(Napi::Env env, const Napi::Value& value, int* out) {
  return ReadWholeHertzSampleRate(env, value, out);
}

bool ReadSessionLimits(Napi::Env env, const Napi::Object& object, uint64_t* max_history_bytes,
                       uint64_t* max_cache_bytes, uint32_t* max_undo_depth,
                       uint32_t* max_render_jobs) {
  const Napi::Value limits_value = object.Get("limits");
  if (limits_value.IsUndefined() || limits_value.IsNull()) return true;
  Napi::Object limits;
  if (!RequiredObjectValue(env, limits_value, "limits", &limits)) return false;
  *max_history_bytes = Uint64Property(limits, "maxHistoryBytes", *max_history_bytes);
  *max_cache_bytes = Uint64Property(limits, "maxCacheBytes", *max_cache_bytes);
  const Napi::Value undo_depth = limits.Get("maxUndoDepth");
  if (!undo_depth.IsUndefined() && !undo_depth.IsNull() &&
      !RequiredUint32Value(env, undo_depth, "maxUndoDepth", max_undo_depth)) {
    return false;
  }
  const Napi::Value render_jobs = limits.Get("maxRenderJobs");
  if (!render_jobs.IsUndefined() && !render_jobs.IsNull() &&
      !RequiredUint32Value(env, render_jobs, "maxRenderJobs", max_render_jobs)) {
    return false;
  }
  return true;
}

bool ReadCreateOptions(Napi::Env env, const Napi::Value& value, SonareVocalCreateOptions* options,
                       SonareVocalAnalysis* analysis_storage, std::vector<float>* f0_storage,
                       std::vector<uint8_t>* voiced_storage, std::string* algorithm_storage) {
  sonare_vocal_create_options_init(options);
  if (value.IsUndefined() || value.IsNull()) return true;
  Napi::Object object;
  if (!RequiredObjectValue(env, value, "options", &object)) return false;
  const auto optional_number = [&](const char* key, double* destination, double minimum,
                                   double maximum) {
    const Napi::Value field = object.Get(key);
    if (field.IsUndefined() || field.IsNull()) return true;
    return RequiredFiniteDoubleValue(env, field, key, destination, minimum, maximum);
  };
  const Napi::Value output_length = object.Get("outputLengthSamples");
  if (!output_length.IsUndefined() && !output_length.IsNull() &&
      !RequiredSafeIntegerValue(env, output_length, "outputLengthSamples",
                                &options->output_length_samples, 0))
    return false;
  if (!optional_number("edgeFadeMs", &options->edge_fade_ms, 0.0,
                       std::numeric_limits<double>::infinity()) ||
      !optional_number("vibratoCutoffHz", &options->vibrato_cutoff_hz, 0.0,
                       std::numeric_limits<double>::infinity()) ||
      !optional_number("segmentationThresholdCents", &options->segmentation_threshold_cents, 0.0,
                       std::numeric_limits<double>::infinity()) ||
      !optional_number("minNoteMs", &options->min_note_ms, 0.0,
                       std::numeric_limits<double>::infinity()) ||
      !optional_number("fminHz", &options->fmin_hz, 0.0, std::numeric_limits<double>::infinity()) ||
      !optional_number("fmaxHz", &options->fmax_hz, 0.0, std::numeric_limits<double>::infinity()) ||
      !optional_number("yinThreshold", &options->yin_threshold, 0.0, 1.0) ||
      !optional_number("voicedThreshold", &options->voiced_threshold, 0.0, 1.0) ||
      !optional_number("referenceHz", &options->reference_hz, 0.0,
                       std::numeric_limits<double>::infinity()))
    return false;
  const Napi::Value centered = object.Get("centered");
  if (!centered.IsUndefined() && !centered.IsNull()) {
    bool flag = false;
    if (!RequiredBoolValue(env, centered, "centered", &flag)) return false;
    options->centered = flag ? 1u : 0u;
  }
  options->frame_length_samples =
      DomainProperty<uint32_t>(object, "frameLengthSamples", options->frame_length_samples,
                               sonare_c_detail::kVocalCreateFrameLengthDomain);
  options->hop_length_samples =
      DomainProperty<uint32_t>(object, "hopLengthSamples", options->hop_length_samples,
                               sonare_c_detail::kVocalCreateHopLengthDomain);
  if (!ReadSessionLimits(env, object, &options->max_history_bytes, &options->max_cache_bytes,
                         &options->max_undo_depth, &options->max_render_jobs)) {
    return false;
  }
  const Napi::Value analysis_value = object.Get("analysis");
  if (!analysis_value.IsUndefined() && !analysis_value.IsNull()) {
    Napi::Object analysis;
    if (!RequiredObjectValue(env, analysis_value, "analysis", &analysis)) return false;
    Napi::Float32Array f0;
    Napi::Uint8Array voiced;
    if (!RequiredFloat32ArrayValue(env, analysis.Get("f0Hz"), "analysis.f0Hz", &f0) ||
        !RequiredUint8ArrayValue(env, analysis.Get("voiced"), "analysis.voiced", &voiced))
      return false;
    if (f0.ElementLength() != voiced.ElementLength()) {
      Napi::RangeError::New(env, "analysis.f0Hz and analysis.voiced must have equal lengths")
          .ThrowAsJavaScriptException();
      return false;
    }
    f0_storage->assign(sonare_node::ElementData(f0),
                       sonare_node::ElementData(f0) + f0.ElementLength());
    voiced_storage->assign(sonare_node::ElementData(voiced),
                           sonare_node::ElementData(voiced) + voiced.ElementLength());
    sonare_vocal_analysis_init(analysis_storage);
    // Omitted analysis settings inherit the create defaults; supplied ones replace them below.
    analysis_storage->fmin_hz = options->fmin_hz;
    analysis_storage->fmax_hz = options->fmax_hz;
    analysis_storage->yin_threshold = options->yin_threshold;
    analysis_storage->voiced_threshold = options->voiced_threshold;
    analysis_storage->centered = options->centered;
    analysis_storage->segmentation_threshold_cents = options->segmentation_threshold_cents;
    analysis_storage->min_note_ms = options->min_note_ms;
    analysis_storage->reference_hz = options->reference_hz;
    if (!RequiredFiniteDoubleValue(env, analysis.Get("frameOriginSample"),
                                   "analysis.frameOriginSample",
                                   &analysis_storage->frame_origin_sample))
      return false;
    if (!RequiredFiniteDoubleValue(env, analysis.Get("samplesPerFrame"), "analysis.samplesPerFrame",
                                   &analysis_storage->samples_per_frame, 1.0,
                                   std::numeric_limits<double>::infinity()))
      return false;
    if (!RequiredUint32Value(env, analysis.Get("frameLengthSamples"), "analysis.frameLengthSamples",
                             &analysis_storage->frame_length_samples))
      return false;
    *algorithm_storage = StringProperty(analysis, "algorithmId", "host");
    if (algorithm_storage->empty()) {
      Napi::RangeError::New(env, "analysis.algorithmId must not be empty")
          .ThrowAsJavaScriptException();
      return false;
    }
    analysis_storage->algorithm_id = algorithm_storage->c_str();
    analysis_storage->algorithm_version = 1;
    const Napi::Value algorithm_version = analysis.Get("algorithmVersion");
    if (!algorithm_version.IsUndefined() && !algorithm_version.IsNull() &&
        !RequiredUint32Value(env, algorithm_version, "analysis.algorithmVersion",
                             &analysis_storage->algorithm_version))
      return false;
    const auto analysis_optional_number = [&](const char* key, double* destination, double minimum,
                                              double maximum) {
      const Napi::Value field = analysis.Get(key);
      if (field.IsUndefined() || field.IsNull()) return true;
      const std::string name = std::string("analysis.") + key;
      return RequiredFiniteDoubleValue(env, field, name.c_str(), destination, minimum, maximum);
    };
    if (!analysis_optional_number("fminHz", &analysis_storage->fmin_hz, 0.0,
                                  std::numeric_limits<double>::infinity()) ||
        !analysis_optional_number("fmaxHz", &analysis_storage->fmax_hz, 0.0,
                                  std::numeric_limits<double>::infinity()) ||
        !analysis_optional_number("yinThreshold", &analysis_storage->yin_threshold, 0.0, 1.0) ||
        !analysis_optional_number("voicedThreshold", &analysis_storage->voiced_threshold, 0.0,
                                  1.0) ||
        !analysis_optional_number("segmentationThresholdCents",
                                  &analysis_storage->segmentation_threshold_cents, 0.0,
                                  std::numeric_limits<double>::infinity()) ||
        !analysis_optional_number("minNoteMs", &analysis_storage->min_note_ms, 0.0,
                                  std::numeric_limits<double>::infinity()) ||
        !analysis_optional_number("referenceHz", &analysis_storage->reference_hz, 0.0,
                                  std::numeric_limits<double>::infinity())) {
      return false;
    }
    const Napi::Value analysis_centered = analysis.Get("centered");
    if (!analysis_centered.IsUndefined() && !analysis_centered.IsNull()) {
      bool flag = false;
      if (!RequiredBoolValue(env, analysis_centered, "analysis.centered", &flag)) return false;
      analysis_storage->centered = flag ? 1u : 0u;
    }
    analysis_storage->f0_hz = f0_storage->data();
    analysis_storage->voiced = voiced_storage->data();
    analysis_storage->frame_count = f0_storage->size();
    options->analysis = analysis_storage;
  }
  return true;
}

}  // namespace sonare_node::vocal_edit_detail
