#include "vocal_edit.h"

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

#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;

bool ReadArrayNumber(Napi::Env env, const Napi::Value& value, const char* field,
                     std::vector<float>* out) {
  out->clear();
  if (value.IsUndefined() || value.IsNull()) return true;
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    const auto array = value.As<Napi::Float32Array>();
    out->assign(array.Data(), array.Data() + array.ElementLength());
  } else if (value.IsArray()) {
    const auto array = value.As<Napi::Array>();
    out->reserve(array.Length());
    for (uint32_t i = 0; i < array.Length(); ++i) {
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
      Napi::RangeError::New(env, std::string(field) + " must contain finite non-negative values")
          .ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

bool ReadStringEnum(Napi::Env env, const Napi::Value& value, const char* field,
                    const char* const* names, size_t count, uint32_t* out) {
  if (!value.IsString()) {
    Napi::TypeError::New(env, std::string(field) + " must be a string")
        .ThrowAsJavaScriptException();
    return false;
  }
  const std::string name = value.As<Napi::String>().Utf8Value();
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
  if (end < start) {
    Napi::RangeError::New(env, "range.endSample must be greater than or equal to startSample")
        .ThrowAsJavaScriptException();
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

struct EditStorage {
  std::vector<SonareVocalPitchPoint> points;
  std::vector<float> envelope;
};

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
    storage->points.reserve(points.Length());
    double previous = -std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < points.Length(); ++i) {
      Napi::Object point;
      if (!RequiredObjectValue(env, points.Get(i), "edit.pitch.target.points[]", &point))
        return false;
      double source_sample = 0.0;
      double midi = 0.0;
      if (!RequiredDoubleProperty(env, point, "sourceSample", &source_sample) ||
          !RequiredDoubleProperty(env, point, "midi", &midi))
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
    out->target_points = storage->points.data();
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

  const Napi::Value destination_start = object.Get("destinationStartSample");
  const Napi::Value destination_length = object.Get("destinationLengthSamples");
  if (!destination_start.IsUndefined() && !destination_start.IsNull() &&
      !RequiredSafeIntegerValue(env, destination_start, "edit.destinationStartSample",
                                &out->destination_start_sample, 0)) {
    return false;
  }
  if (!destination_length.IsUndefined() && !destination_length.IsNull() &&
      !RequiredSafeIntegerValue(env, destination_length, "edit.destinationLengthSamples",
                                &out->destination_length_samples, 0)) {
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

struct ParsedOperation {
  SonareVocalOperation operation{};
  EditStorage edit_storage;
  std::vector<uint32_t> note_ids;
};

bool ReadTransition(Napi::Env env, const Napi::Value& value, SonareVocalTransition* out) {
  Napi::Object object;
  if (!RequiredObjectValue(env, value, "transition", &object)) return false;
  *out = {};
  out->struct_size = sizeof(*out);
  out->schema_version = SONARE_VOCAL_EDIT_API_VERSION;
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
      curve.As<Napi::String>().Utf8Value() != "smoothstep") {
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
  parsed->reserve(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) {
    Napi::Object object;
    if (!RequiredObjectValue(env, array.Get(i), "operations[]", &object)) return false;
    const Napi::Value kind_value = object.Get("kind");
    if (!kind_value.IsString()) {
      Napi::TypeError::New(env, "operation.kind must be a string").ThrowAsJavaScriptException();
      return false;
    }
    ParsedOperation item;
    sonare_vocal_operation_init(&item.operation);
    const std::string kind = kind_value.As<Napi::String>().Utf8Value();
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
                                    &item.operation.cut_source_sample, 0)) {
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
      item.note_ids.reserve(ids.Length());
      for (uint32_t j = 0; j < ids.Length(); ++j) {
        uint32_t id = 0;
        if (!ReadNoteId(env, ids.Get(j), "operation.noteIds[]", &id)) return false;
        item.note_ids.push_back(id);
      }
      item.operation.note_ids = item.note_ids.data();
      item.operation.note_id_count = item.note_ids.size();
      const Napi::Value policy = object.Get("policy");
      if (policy.IsUndefined() || policy.IsNull()) {
        item.operation.merge_policy = SONARE_VOCAL_MERGE_PRESERVE;
      } else {
        const char* const policy_names[] = {"preserve", "reset"};
        if (!ReadStringEnum(env, policy, "operation.policy", policy_names, std::size(policy_names),
                            &item.operation.merge_policy))
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
      item.note_ids.reserve(ids.Length());
      for (uint32_t j = 0; j < ids.Length(); ++j) {
        uint32_t id = 0;
        if (!ReadNoteId(env, ids.Get(j), "operation.noteIds[]", &id)) return false;
        item.note_ids.push_back(id);
      }
      item.operation.note_ids = item.note_ids.data();
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

Napi::Object EditToObject(Napi::Env env, const SonareVocalNoteEdit& edit) {
  Napi::Object pitch = Napi::Object::New(env);
  Napi::Object target = Napi::Object::New(env);
  const char* const target_names[] = {"none", "center", "curve"};
  target.Set("mode", target_names[std::min<uint32_t>(edit.target_mode, 2u)]);
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
  formant.Set("mode", formant_names[std::min<uint32_t>(edit.formant_mode, 1u)]);
  formant.Set("shiftSemitones", edit.formant_shift_semitones);
  Napi::Object result = Napi::Object::New(env);
  result.Set("pitch", pitch);
  result.Set("destinationStartSample",
             Napi::Number::New(env, static_cast<double>(edit.destination_start_sample)));
  result.Set("destinationLengthSamples",
             Napi::Number::New(env, static_cast<double>(edit.destination_length_samples)));
  result.Set("gainDb", edit.gain_db);
  result.Set("muted", Napi::Boolean::New(env, edit.muted != 0));
  Napi::Float32Array envelope = Napi::Float32Array::New(env, edit.amplitude_envelope_count);
  if (edit.amplitude_envelope_count != 0) {
    std::memcpy(envelope.Data(), edit.amplitude_envelope,
                edit.amplitude_envelope_count * sizeof(float));
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
  Napi::Float32Array amplitude = Napi::Float32Array::New(env, note.amplitude_count);
  if (note.amplitude_count != 0) {
    std::memcpy(amplitude.Data(), note.amplitude, note.amplitude_count * sizeof(float));
  }
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
    std::memcpy(f0.Data(), result.analysis.f0_hz, result.analysis.frame_count * sizeof(float));
    std::memcpy(voiced.Data(), result.analysis.voiced, result.analysis.frame_count);
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
  object.Set("apiVersion", capabilities.api_version);
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
    std::memcpy(source.Data(), result.source_samples, result.frame_count * sizeof(double));
    std::memcpy(measured.Data(), result.measured_midi, result.frame_count * sizeof(double));
    std::memcpy(target.Data(), result.target_midi, result.frame_count * sizeof(double));
    std::memcpy(effective.Data(), result.effective_midi, result.frame_count * sizeof(double));
    std::memcpy(voiced.Data(), result.voiced, result.frame_count);
    std::memcpy(has_target.Data(), result.has_target, result.frame_count);
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
  Napi::Float32Array samples = Napi::Float32Array::New(env, result.sample_count);
  if (result.sample_count != 0) {
    std::memcpy(samples.Data(), result.samples,
                static_cast<size_t>(result.sample_count) * sizeof(float));
  }
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
  const char* message = sonare_error_message(error);
  Napi::Error js_error = Napi::Error::New(env, message == nullptr ? "libsonare error" : message);
  Napi::Object object = js_error.Value();
  object.Set("name", "SonareError");
  object.Set("code", Napi::Number::New(env, static_cast<int>(error)));
  object.Set("codeName", sonare_node::ErrorCodeName(error));
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
struct EditGuard {
  SonareVocalEditResult value{};
  EditGuard() { sonare_vocal_edit_result_init(&value); }
  ~EditGuard() { sonare_vocal_free_edit_result(&value); }
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

bool ReadSampleRate(Napi::Env env, const Napi::Value& value, int* out) {
  int64_t rate = 0;
  if (!RequiredSafeIntegerValue(env, value, "sampleRate", &rate, 0) || rate < 8000 ||
      rate > 384000) {
    if (!env.IsExceptionPending()) {
      Napi::RangeError::New(env, "sampleRate must be an integer in [8000, 384000]")
          .ThrowAsJavaScriptException();
    }
    return false;
  }
  *out = static_cast<int>(rate);
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
  const Napi::Value frame_length = object.Get("frameLengthSamples");
  if (!frame_length.IsUndefined() && !frame_length.IsNull() &&
      !RequiredUint32Value(env, frame_length, "frameLengthSamples",
                           &options->frame_length_samples)) {
    return false;
  }
  const Napi::Value hop_length = object.Get("hopLengthSamples");
  if (!hop_length.IsUndefined() && !hop_length.IsNull() &&
      !RequiredUint32Value(env, hop_length, "hopLengthSamples", &options->hop_length_samples)) {
    return false;
  }
  Napi::Object limits;
  const Napi::Value limits_value = object.Get("limits");
  if (!limits_value.IsUndefined() && !limits_value.IsNull()) {
    if (!RequiredObjectValue(env, limits_value, "limits", &limits)) return false;
    options->max_history_bytes =
        Uint64Property(limits, "maxHistoryBytes", options->max_history_bytes);
    options->max_cache_bytes = Uint64Property(limits, "maxCacheBytes", options->max_cache_bytes);
    const Napi::Value max_undo_depth = limits.Get("maxUndoDepth");
    if (!max_undo_depth.IsUndefined() && !max_undo_depth.IsNull() &&
        !RequiredUint32Value(env, max_undo_depth, "maxUndoDepth", &options->max_undo_depth)) {
      return false;
    }
    const Napi::Value max_render_jobs = limits.Get("maxRenderJobs");
    if (!max_render_jobs.IsUndefined() && !max_render_jobs.IsNull() &&
        !RequiredUint32Value(env, max_render_jobs, "maxRenderJobs", &options->max_render_jobs)) {
      return false;
    }
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
    f0_storage->assign(f0.Data(), f0.Data() + f0.ElementLength());
    voiced_storage->assign(voiced.Data(), voiced.Data() + voiced.ElementLength());
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
    *algorithm_storage = StringProperty(analysis, "algorithmId", "libsonare.pyin");
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

}  // namespace

Napi::FunctionReference VocalEditSessionWrap::constructor_;
Napi::FunctionReference VocalEditDraftWrap::constructor_;
Napi::FunctionReference VocalRenderSnapshotWrap::constructor_;
Napi::FunctionReference VocalRenderJobWrap::constructor_;

namespace {

Napi::Value RequireSessionValue(Napi::Env env, SonareVocalEditSession* session) {
  if (session != nullptr) return env.Undefined();
  CheckVocalError(env, SONARE_ERROR_INVALID_STATE);
  return env.Undefined();
}

class VocalRenderAsyncWorker final : public Napi::AsyncWorker {
 public:
  VocalRenderAsyncWorker(Napi::Env env, SonareVocalRenderJob* job, const Napi::Object& snapshot,
                         std::shared_ptr<std::atomic_bool> cancelled, Napi::Object signal,
                         Napi::Function listener)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(job),
        snapshot_(Napi::Persistent(snapshot)),
        cancelled_(std::move(cancelled)),
        signal_(signal.IsEmpty() ? Napi::ObjectReference{} : Napi::Persistent(signal)),
        listener_(listener.IsEmpty() ? Napi::FunctionReference{} : Napi::Persistent(listener)) {}

  ~VocalRenderAsyncWorker() override {
    if (result_initialized_) {
      sonare_vocal_free_render_result(&result_);
      result_initialized_ = false;
    }
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    int complete = 0;
    while (!complete) {
      const SonareError error = sonare_vocal_render_job_next(
          job_, &VocalRenderAsyncWorker::CancelCallback, cancelled_.get(), &complete);
      if (error != SONARE_OK) {
        CaptureError(error);
        return;
      }
    }
    sonare_vocal_render_result_init(&result_);
    result_initialized_ = true;
    const SonareError error = sonare_vocal_render_job_finalize(
        job_, &VocalRenderAsyncWorker::CancelCallback, cancelled_.get(), &result_);
    if (error != SONARE_OK) {
      sonare_vocal_free_render_result(&result_);
      CaptureError(error);
      return;
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    DetachSignal();
    deferred_.Resolve(RenderResultToObject(Env(), result_));
    sonare_vocal_free_render_result(&result_);
    result_initialized_ = false;
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    DetachSignal();
    Napi::Object value = error.Value();
    value.Set("name", "SonareError");
    value.Set("code", Napi::Number::New(Env(), static_cast<int>(error_code_)));
    value.Set("codeName", sonare_node::ErrorCodeName(error_code_));
    value.Set("reason", detail_.reason);
    value.Set("field", detail_.field);
    value.Set("expected", detail_.expected_text[0] == '\0' ? Uint64String(detail_.expected)
                                                           : detail_.expected_text);
    value.Set("actual",
              detail_.actual_text[0] == '\0' ? Uint64String(detail_.actual) : detail_.actual_text);
    value.Set("expectedText", detail_.expected_text);
    value.Set("actualText", detail_.actual_text);
    deferred_.Reject(value);
    if (result_initialized_) {
      sonare_vocal_free_render_result(&result_);
      result_initialized_ = false;
    }
    if (job_ != nullptr) {
      sonare_vocal_render_job_destroy(job_);
      job_ = nullptr;
    }
  }

 private:
  static int CancelCallback(void* user_data) {
    return static_cast<std::atomic_bool*>(user_data)->load(std::memory_order_relaxed) ? 1 : 0;
  }

  void CaptureError(SonareError error) {
    error_code_ = error;
    sonare_vocal_error_detail_init(&detail_);
    sonare_vocal_last_error_detail(&detail_);
    SetError(sonare_error_message(error) == nullptr ? "vocal render failed"
                                                    : sonare_error_message(error));
  }

  void DetachSignal() {
    const Napi::Env env = Env();
    const bool had_pending_exception = env.IsExceptionPending();
    if (!had_pending_exception && !signal_.IsEmpty() && !listener_.IsEmpty()) {
      const Napi::Value remove = signal_.Value().Get("removeEventListener");
      if (!env.IsExceptionPending() && remove.IsFunction()) {
        remove.As<Napi::Function>().Call(signal_.Value(),
                                         {Napi::String::New(env, "abort"), listener_.Value()});
      }
      // Swallow detach errors so a throwing AbortSignal cannot poison the env before cleanup.
      if (env.IsExceptionPending()) {
        env.GetAndClearPendingException();
      }
    }
    listener_.Reset();
    signal_.Reset();
    snapshot_.Reset();
  }

  Napi::Promise::Deferred deferred_;
  SonareVocalRenderJob* job_ = nullptr;
  Napi::ObjectReference snapshot_;
  std::shared_ptr<std::atomic_bool> cancelled_;
  Napi::ObjectReference signal_;
  Napi::FunctionReference listener_;
  SonareVocalRenderResult result_{};
  bool result_initialized_ = false;
  SonareError error_code_ = SONARE_ERROR_UNKNOWN;
  SonareVocalErrorDetail detail_{};
};

struct VocalRenderJobDeleter {
  void operator()(SonareVocalRenderJob* job) const noexcept {
    if (job != nullptr) sonare_vocal_render_job_destroy(job);
  }
};

bool PrepareAbortSignal(Napi::Env env, const Napi::Value& value,
                        const std::shared_ptr<std::atomic_bool>& cancelled,
                        Napi::Object* signal_out, Napi::Function* listener_out) {
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!value.IsObject()) {
    Napi::TypeError::New(env, "signal must be an AbortSignal-like object")
        .ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Object signal = value.As<Napi::Object>();
  const Napi::Value aborted = signal.Get("aborted");
  if (!aborted.IsUndefined() && !aborted.IsBoolean()) {
    Napi::TypeError::New(env, "signal.aborted must be a boolean").ThrowAsJavaScriptException();
    return false;
  }
  if (aborted.IsBoolean() && aborted.As<Napi::Boolean>().Value()) {
    cancelled->store(true, std::memory_order_relaxed);
  }
  const Napi::Value add = signal.Get("addEventListener");
  if (!add.IsFunction()) {
    Napi::TypeError::New(env, "signal must provide addEventListener").ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Function listener = Napi::Function::New(env, [cancelled](const Napi::CallbackInfo&) {
    cancelled->store(true, std::memory_order_relaxed);
  });
  add.As<Napi::Function>().Call(signal, {Napi::String::New(env, "abort"), listener});
  if (env.IsExceptionPending()) return false;
  *signal_out = signal;
  *listener_out = listener;
  return true;
}

}  // namespace

Napi::Object VocalEditSessionWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalEditSession",
      {
          InstanceMethod<&VocalEditSessionWrap::Notes>("notes"),
          InstanceMethod<&VocalEditSessionWrap::Analysis>("analysis"),
          InstanceMethod<&VocalEditSessionWrap::Capabilities>("capabilities"),
          InstanceMethod<&VocalEditSessionWrap::Token>("token"),
          InstanceMethod<&VocalEditSessionWrap::Revision>("revision"),
          InstanceMethod<&VocalEditSessionWrap::OutputLengthSamples>("outputLengthSamples"),
          InstanceMethod<&VocalEditSessionWrap::History>("history"),
          InstanceMethod<&VocalEditSessionWrap::BeginEdit>("beginEdit"),
          InstanceMethod<&VocalEditSessionWrap::Undo>("undo"),
          InstanceMethod<&VocalEditSessionWrap::Redo>("redo"),
          InstanceMethod<&VocalEditSessionWrap::EvaluatePitch>("evaluatePitch"),
          InstanceMethod<&VocalEditSessionWrap::SourceSampleToDestinationSample>(
              "sourceSampleToDestinationSample"),
          InstanceMethod<&VocalEditSessionWrap::DestinationSampleToSourceSample>(
              "destinationSampleToSourceSample"),
          InstanceMethod<&VocalEditSessionWrap::CaptureRenderSnapshot>("captureRenderSnapshot"),
          InstanceMethod<&VocalEditSessionWrap::ExportState>("exportState"),
          InstanceMethod<&VocalEditSessionWrap::Destroy>("destroy"),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalEditSession", function);
  return exports;
}

Napi::Object VocalEditSessionWrap::NewInstance(Napi::Env env, SonareVocalEditSession* session) {
  (void)env;
  Napi::Object object = constructor_.New({});
  VocalEditSessionWrap::Unwrap(object)->session_ = session;
  return object;
}

VocalEditSessionWrap::VocalEditSessionWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalEditSessionWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalEditSessionWrap::~VocalEditSessionWrap() {
  if (session_ != nullptr) sonare_vocal_session_destroy(session_);
  session_ = nullptr;
}

Napi::Value VocalEditSessionWrap::Notes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  NotesGuard notes;
  const SonareError error = sonare_vocal_session_notes(session_, &notes.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return NotesResultToObject(env, notes.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Analysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  AnalysisGuard analysis;
  const SonareError error = sonare_vocal_session_analysis(session_, &analysis.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return AnalysisResultToObject(env, analysis.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Capabilities(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalCapabilities capabilities{};
  sonare_vocal_capabilities_init(&capabilities);
  const SonareError error = sonare_vocal_session_capabilities(session_, &capabilities);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return CapabilitiesToObject(env, capabilities);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Token(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalStateToken token{};
  const SonareError error = sonare_vocal_session_token(session_, &token);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return TokenToObject(env, token);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Revision(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  const SonareError error = sonare_vocal_session_revision(session_, &revision);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::String::New(env, Uint64String(revision));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::OutputLengthSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  int64_t samples = 0;
  const SonareError error = sonare_vocal_session_output_length(session_, &samples);
  if (!CheckVocalError(env, error)) return env.Undefined();
  if (samples < 0 || static_cast<double>(samples) > kMaxSafeInteger) {
    Napi::RangeError::New(env, "output length cannot be represented as a JavaScript number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(samples));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::History(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  int can_undo = 0;
  int can_redo = 0;
  const SonareError error = sonare_vocal_session_history(session_, &can_undo, &can_redo);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("canUndo", Napi::Boolean::New(env, can_undo != 0));
  result.Set("canRedo", Napi::Boolean::New(env, can_redo != 0));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::BeginEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if (info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) {
    if (!CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
      return env.Undefined();
    }
  }
  SonareVocalEditDraft* draft = nullptr;
  const SonareError error = sonare_vocal_session_begin_edit(session_, revision, &draft);
  if (!CheckVocalError(env, error)) return env.Undefined();
  RetainDraft();
  return VocalEditDraftWrap::NewInstance(env, draft, this, info.This().As<Napi::Object>());
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Undo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) &&
      !CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_session_undo(session_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::Redo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) &&
      !CheckVocalError(env, sonare_vocal_session_revision(session_, &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_session_redo(session_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::EvaluatePitch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint32_t note_id = 0;
  if (info.Length() == 0 || !ReadNoteId(env, info[0], "noteId", &note_id)) return env.Undefined();
  PitchGuard result;
  const SonareError error = sonare_vocal_session_evaluate_pitch(session_, note_id, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return PitchResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::SourceSampleToDestinationSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint32_t note_id = 0;
  double source_sample = 0.0;
  if (info.Length() < 2 || !ReadNoteId(env, info[0], "noteId", &note_id) ||
      !RequiredFiniteDoubleValue(env, info[1], "sourceSample", &source_sample))
    return env.Undefined();
  double destination_sample = 0.0;
  const SonareError error = sonare_vocal_session_source_to_destination(
      session_, note_id, source_sample, &destination_sample);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::Number::New(env, destination_sample);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::DestinationSampleToSourceSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  uint32_t note_id = 0;
  double destination_sample = 0.0;
  if (info.Length() < 2 || !ReadNoteId(env, info[0], "noteId", &note_id) ||
      !RequiredFiniteDoubleValue(env, info[1], "destinationSample", &destination_sample))
    return env.Undefined();
  double source_sample = 0.0;
  const SonareError error = sonare_vocal_session_destination_to_source(
      session_, note_id, destination_sample, &source_sample);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::Number::New(env, source_sample);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::CaptureRenderSnapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  SonareVocalRenderSnapshot* snapshot = nullptr;
  const SonareError error = sonare_vocal_session_capture_snapshot(session_, &snapshot);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderSnapshotWrap::NewInstance(env, snapshot);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditSessionWrap::ExportState(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return RequireSessionValue(env, session_);
  StateGuard state;
  const SonareError error = sonare_vocal_session_export_state(session_, &state.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Uint8Array result = Napi::Uint8Array::New(env, state.value.size);
  if (state.value.size != 0) std::memcpy(result.Data(), state.value.data, state.value.size);
  return result;
  SONARE_NODE_CATCH(env)
}

void VocalEditSessionWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (session_ == nullptr) return;
  if (draft_count_ != 0) {
    Napi::Error::New(env, "VocalEditSession has live drafts").ThrowAsJavaScriptException();
    return;
  }
  sonare_vocal_session_destroy(session_);
  session_ = nullptr;
  destroyed_ = true;
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalEditDraftWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalEditDraft",
      {
          InstanceMethod<&VocalEditDraftWrap::Notes>("notes"),
          InstanceMethod<&VocalEditDraftWrap::Token>("token"),
          InstanceMethod<&VocalEditDraftWrap::Apply>("apply"),
          InstanceMethod<&VocalEditDraftWrap::Commit>("commit"),
          InstanceMethod<&VocalEditDraftWrap::Cancel>("cancel"),
          InstanceMethod<&VocalEditDraftWrap::EvaluatePitch>("evaluatePitch"),
          InstanceMethod<&VocalEditDraftWrap::SourceSampleToDestinationSample>(
              "sourceSampleToDestinationSample"),
          InstanceMethod<&VocalEditDraftWrap::DestinationSampleToSourceSample>(
              "destinationSampleToSourceSample"),
          InstanceMethod<&VocalEditDraftWrap::CaptureRenderSnapshot>("captureRenderSnapshot"),
          InstanceMethod<&VocalEditDraftWrap::Destroy>("destroy"),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalEditDraft", function);
  return exports;
}

Napi::Object VocalEditDraftWrap::NewInstance(Napi::Env env, SonareVocalEditDraft* draft,
                                             VocalEditSessionWrap* owner,
                                             const Napi::Object& owner_object) {
  (void)env;
  Napi::Object object = constructor_.New({});
  auto* wrapper = VocalEditDraftWrap::Unwrap(object);
  wrapper->draft_ = draft;
  wrapper->owner_ = owner;
  wrapper->owner_object_ = Napi::Persistent(owner_object);
  return object;
}

VocalEditDraftWrap::VocalEditDraftWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalEditDraftWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalEditDraftWrap::~VocalEditDraftWrap() {
  if (draft_ != nullptr) {
    sonare_vocal_draft_destroy(draft_);
    draft_ = nullptr;
  }
  if (owner_ != nullptr && !released_owner_) owner_->ReleaseDraft();
  owner_ = nullptr;
  owner_object_.Reset();
}

Napi::Value VocalEditDraftWrap::Notes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  NotesGuard notes;
  const SonareError error = sonare_vocal_draft_notes(draft_, &notes.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return NotesResultToObject(env, notes.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Token(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalStateToken token{};
  const SonareError error = sonare_vocal_draft_token(draft_, &token);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return TokenToObject(env, token);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Apply(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "apply(expectedGeneration, operations) requires two arguments")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint64_t generation = 0;
  if (!RequiredUint64Value(env, info[0], "expectedGeneration", &generation)) return env.Undefined();
  std::vector<ParsedOperation> parsed;
  if (!ReadOperations(env, info[1], &parsed)) return env.Undefined();
  std::vector<SonareVocalOperation> operations;
  operations.reserve(parsed.size());
  for (const ParsedOperation& item : parsed) operations.push_back(item.operation);
  EditGuard result;
  const SonareError error =
      sonare_vocal_draft_apply(draft_, generation, operations.empty() ? nullptr : operations.data(),
                               operations.size(), &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Commit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint64_t revision = 0;
  if (info.Length() != 0 && !info[0].IsUndefined() && !info[0].IsNull() &&
      !RequiredUint64Value(env, info[0], "expectedRevision", &revision)) {
    return env.Undefined();
  }
  if ((info.Length() == 0 || info[0].IsUndefined() || info[0].IsNull()) && owner_ != nullptr &&
      owner_->native() != nullptr &&
      !CheckVocalError(env, sonare_vocal_session_revision(owner_->native(), &revision))) {
    return env.Undefined();
  }
  EditGuard result;
  const SonareError error = sonare_vocal_draft_commit(draft_, revision, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  sonare_vocal_draft_destroy(draft_);
  draft_ = nullptr;
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft();
    released_owner_ = true;
  }
  owner_object_.Reset();
  return EditResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::Cancel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return env.Undefined();
  const SonareError error = sonare_vocal_draft_cancel(draft_);
  if (!CheckVocalError(env, error)) return env.Undefined();
  sonare_vocal_draft_destroy(draft_);
  draft_ = nullptr;
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft();
    released_owner_ = true;
  }
  owner_object_.Reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::EvaluatePitch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint32_t note_id = 0;
  if (info.Length() == 0 || !ReadNoteId(env, info[0], "noteId", &note_id)) return env.Undefined();
  PitchGuard result;
  const SonareError error = sonare_vocal_draft_evaluate_pitch(draft_, note_id, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return PitchResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::SourceSampleToDestinationSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint32_t note_id = 0;
  double source_sample = 0.0;
  if (info.Length() < 2 || !ReadNoteId(env, info[0], "noteId", &note_id) ||
      !RequiredFiniteDoubleValue(env, info[1], "sourceSample", &source_sample))
    return env.Undefined();
  double destination_sample = 0.0;
  const SonareError error =
      sonare_vocal_draft_source_to_destination(draft_, note_id, source_sample, &destination_sample);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::Number::New(env, destination_sample);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::DestinationSampleToSourceSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  uint32_t note_id = 0;
  double destination_sample = 0.0;
  if (info.Length() < 2 || !ReadNoteId(env, info[0], "noteId", &note_id) ||
      !RequiredFiniteDoubleValue(env, info[1], "destinationSample", &destination_sample))
    return env.Undefined();
  double source_sample = 0.0;
  const SonareError error =
      sonare_vocal_draft_destination_to_source(draft_, note_id, destination_sample, &source_sample);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return Napi::Number::New(env, source_sample);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditDraftWrap::CaptureRenderSnapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ == nullptr) return RequireSessionValue(env, nullptr);
  SonareVocalRenderSnapshot* snapshot = nullptr;
  const SonareError error = sonare_vocal_draft_capture_snapshot(draft_, &snapshot);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderSnapshotWrap::NewInstance(env, snapshot);
  SONARE_NODE_CATCH(env)
}

void VocalEditDraftWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (draft_ != nullptr) {
    sonare_vocal_draft_destroy(draft_);
    draft_ = nullptr;
  }
  if (owner_ != nullptr && !released_owner_) {
    owner_->ReleaseDraft();
    released_owner_ = true;
  }
  owner_object_.Reset();
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalRenderSnapshotWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function = DefineClass(
      env, "VocalRenderSnapshot",
      {
          InstanceMethod<&VocalRenderSnapshotWrap::Render>("render"),
          InstanceMethod<&VocalRenderSnapshotWrap::RenderAsync>("renderAsync"),
          InstanceMethod<&VocalRenderSnapshotWrap::BeginRenderJob>("beginRenderJob"),
          InstanceMethod<&VocalRenderSnapshotWrap::OutputLengthSamples>("outputLengthSamples"),
          InstanceMethod<&VocalRenderSnapshotWrap::Destroy>("destroy"),
      });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalRenderSnapshot", function);
  return exports;
}

Napi::Object VocalRenderSnapshotWrap::NewInstance(Napi::Env env,
                                                  SonareVocalRenderSnapshot* snapshot) {
  (void)env;
  Napi::Object object = constructor_.New({});
  VocalRenderSnapshotWrap::Unwrap(object)->snapshot_ = snapshot;
  return object;
}

VocalRenderSnapshotWrap::VocalRenderSnapshotWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalRenderSnapshotWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalRenderSnapshotWrap::~VocalRenderSnapshotWrap() {
  if (snapshot_ != nullptr) sonare_vocal_snapshot_destroy(snapshot_);
  snapshot_ = nullptr;
}

Napi::Value VocalRenderSnapshotWrap::OutputLengthSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  int64_t samples = 0;
  const SonareError error = sonare_vocal_snapshot_output_length(snapshot_, &samples);
  if (!CheckVocalError(env, error)) return env.Undefined();
  if (samples < 0 || static_cast<double>(samples) > kMaxSafeInteger) {
    Napi::RangeError::New(env, "output length cannot be represented as a JavaScript number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(samples));
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderSnapshotWrap::Render(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  if (info.Length() == 0) {
    Napi::TypeError::New(env, "render(range, requestId?) requires a range")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareVocalRange range{};
  if (!ReadRange(env, info[0], &range)) return env.Undefined();
  uint64_t request_id = 0;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull() &&
      !RequiredUint64Value(env, info[1], "requestId", &request_id)) {
    return env.Undefined();
  }
  RenderGuard result;
  const SonareError error =
      sonare_vocal_snapshot_render(snapshot_, range, request_id, nullptr, nullptr, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return RenderResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderSnapshotWrap::RenderAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RejectedVocalPromise(env, SONARE_ERROR_INVALID_STATE);
  if (info.Length() == 0) {
    Napi::TypeError::New(env, "renderAsync(range, requestId?, signal?) requires a range")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareVocalRange range{};
  if (!ReadRange(env, info[0], &range)) return env.Undefined();
  uint64_t request_id = 0;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull() &&
      !RequiredUint64Value(env, info[1], "requestId", &request_id)) {
    return env.Undefined();
  }
  SonareVocalRenderJob* job = nullptr;
  const SonareError begin_error = sonare_vocal_render_job_begin(snapshot_, range, request_id, &job);
  if (begin_error != SONARE_OK) return RejectedVocalPromise(env, begin_error);
  std::unique_ptr<SonareVocalRenderJob, VocalRenderJobDeleter> job_owner(job);

  const auto cancelled = std::make_shared<std::atomic_bool>(false);
  Napi::Object signal;
  Napi::Function listener;
  if (info.Length() > 2 && !info[2].IsUndefined() && !info[2].IsNull() &&
      !PrepareAbortSignal(env, info[2], cancelled, &signal, &listener)) {
    return env.Undefined();
  }
  auto worker = std::make_unique<VocalRenderAsyncWorker>(
      env, job_owner.get(), info.This().As<Napi::Object>(), cancelled, signal, listener);
  job_owner.release();
  Napi::Promise promise = worker->Promise();
  worker->Queue();
  if (env.IsExceptionPending()) return env.Undefined();
  worker.release();
  return promise;
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderSnapshotWrap::BeginRenderJob(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ == nullptr) return RequireSessionValue(env, nullptr);
  if (info.Length() == 0) {
    Napi::TypeError::New(env, "beginRenderJob(range, requestId?) requires a range")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareVocalRange range{};
  if (!ReadRange(env, info[0], &range)) return env.Undefined();
  uint64_t request_id = 0;
  if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull() &&
      !RequiredUint64Value(env, info[1], "requestId", &request_id)) {
    return env.Undefined();
  }
  SonareVocalRenderJob* job = nullptr;
  const SonareError error = sonare_vocal_render_job_begin(snapshot_, range, request_id, &job);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalRenderJobWrap::NewInstance(env, job, this, info.This().As<Napi::Object>());
  SONARE_NODE_CATCH(env)
}

void VocalRenderSnapshotWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (snapshot_ != nullptr) {
    sonare_vocal_snapshot_destroy(snapshot_);
    snapshot_ = nullptr;
  }
  destroyed_ = true;
  SONARE_NODE_CATCH_VOID(env)
}

Napi::Object VocalRenderJobWrap::Init(Napi::Env env, Napi::Object exports) {
  const Napi::Function function =
      DefineClass(env, "VocalRenderJob",
                  {
                      InstanceMethod<&VocalRenderJobWrap::Next>("next"),
                      InstanceMethod<&VocalRenderJobWrap::Finish>("finalize"),
                      InstanceMethod<&VocalRenderJobWrap::Abort>("abort"),
                      InstanceMethod<&VocalRenderJobWrap::Destroy>("destroy"),
                  });
  constructor_ = Napi::Persistent(function);
  constructor_.SuppressDestruct();
  exports.Set("VocalRenderJob", function);
  return exports;
}

Napi::Object VocalRenderJobWrap::NewInstance(Napi::Env env, SonareVocalRenderJob* job,
                                             VocalRenderSnapshotWrap* owner,
                                             const Napi::Object& owner_object) {
  (void)env;
  Napi::Object object = constructor_.New({});
  auto* wrapper = VocalRenderJobWrap::Unwrap(object);
  wrapper->job_ = job;
  wrapper->owner_ = owner;
  wrapper->owner_object_ = Napi::Persistent(owner_object);
  return object;
}

VocalRenderJobWrap::VocalRenderJobWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<VocalRenderJobWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_CATCH_VOID(env)
}

VocalRenderJobWrap::~VocalRenderJobWrap() {
  if (job_ != nullptr) sonare_vocal_render_job_destroy(job_);
  job_ = nullptr;
  owner_ = nullptr;
  owner_object_.Reset();
}

Napi::Value VocalRenderJobWrap::Next(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ == nullptr) return RequireSessionValue(env, nullptr);
  int complete = 0;
  const SonareError error = sonare_vocal_render_job_next(job_, nullptr, nullptr, &complete);
  if (!CheckVocalError(env, error)) return env.Undefined();
  Napi::Object result = Napi::Object::New(env);
  result.Set("complete", Napi::Boolean::New(env, complete != 0));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalRenderJobWrap::Finish(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ == nullptr) return RequireSessionValue(env, nullptr);
  RenderGuard result;
  const SonareError error = sonare_vocal_render_job_finalize(job_, nullptr, nullptr, &result.value);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return RenderResultToObject(env, result.value);
  SONARE_NODE_CATCH(env)
}

void VocalRenderJobWrap::Abort(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ != nullptr) sonare_vocal_render_job_abort(job_);
  SONARE_NODE_CATCH_VOID(env)
}

void VocalRenderJobWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (job_ != nullptr) {
    sonare_vocal_render_job_destroy(job_);
    job_ = nullptr;
  }
  owner_ = nullptr;
  owner_object_.Reset();
  SONARE_NODE_CATCH_VOID(env)
}

namespace {

Napi::Value CreateVocalEditSession(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() == 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "createVocalEditSession requires an options object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object request = info[0].As<Napi::Object>();
  Napi::Float32Array samples;
  if (!RequiredFloat32ArrayValue(env, request.Get("samples"), "samples", &samples))
    return env.Undefined();
  int sample_rate = 0;
  if (!ReadSampleRate(env, request.Get("sampleRate"), &sample_rate)) return env.Undefined();
  SonareVocalCreateOptions options{};
  SonareVocalAnalysis analysis{};
  std::vector<float> f0_storage;
  std::vector<uint8_t> voiced_storage;
  std::string algorithm_storage;
  if (!ReadCreateOptions(env, request, &options, &analysis, &f0_storage, &voiced_storage,
                         &algorithm_storage)) {
    return env.Undefined();
  }
  SonareVocalEditSession* session = nullptr;
  const SonareError error =
      sonare_vocal_session_create(samples.Data(), static_cast<int64_t>(samples.ElementLength()), 1,
                                  sample_rate, &options, &session);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalEditSessionWrap::NewInstance(env, session);
  SONARE_NODE_CATCH(env)
}

Napi::Value RestoreVocalEditSession(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() == 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "restoreVocalEditSession requires an options object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object request = info[0].As<Napi::Object>();
  Napi::Float32Array samples;
  Napi::Uint8Array state;
  if (!RequiredFloat32ArrayValue(env, request.Get("samples"), "samples", &samples) ||
      !RequiredUint8ArrayValue(env, request.Get("state"), "state", &state))
    return env.Undefined();
  int sample_rate = 0;
  if (!ReadSampleRate(env, request.Get("sampleRate"), &sample_rate)) return env.Undefined();
  SonareVocalEditSession* session = nullptr;
  const SonareError error =
      sonare_vocal_session_restore(samples.Data(), static_cast<int64_t>(samples.ElementLength()), 1,
                                   sample_rate, state.Data(), state.ElementLength(), &session);
  if (!CheckVocalError(env, error)) return env.Undefined();
  return VocalEditSessionWrap::NewInstance(env, session);
  SONARE_NODE_CATCH(env)
}

Napi::Value VocalEditAvailable(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), sonare_vocal_available() != 0);
}

Napi::Value VocalEditApiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_vocal_edit_api_version());
}

}  // namespace

Napi::Object InitVocalEdit(Napi::Env env, Napi::Object exports) {
  VocalEditSessionWrap::Init(env, exports);
  VocalEditDraftWrap::Init(env, exports);
  VocalRenderSnapshotWrap::Init(env, exports);
  VocalRenderJobWrap::Init(env, exports);
  exports.Set("vocalEditAvailable",
              Napi::Function::New(env, VocalEditAvailable, "vocalEditAvailable"));
  exports.Set("vocalEditApiVersion",
              Napi::Function::New(env, VocalEditApiVersion, "vocalEditApiVersion"));
  exports.Set("createVocalEditSession",
              Napi::Function::New(env, CreateVocalEditSession, "createVocalEditSession"));
  exports.Set("restoreVocalEditSession",
              Napi::Function::New(env, RestoreVocalEditSession, "restoreVocalEditSession"));
  return exports;
}

}  // namespace sonare_node
