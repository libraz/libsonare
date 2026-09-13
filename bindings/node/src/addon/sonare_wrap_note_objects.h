#ifndef SONARE_NODE_SONARE_WRAP_NOTE_OBJECTS_H_
#define SONARE_NODE_SONARE_WRAP_NOTE_OBJECTS_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sonare_wrap_options.h"

namespace sonare_node {

/// @brief Copies @p count floats into a fresh Float32Array; a NULL source reads
///        as zeros rather than being read past.
inline Napi::Float32Array CopyToFloat32(Napi::Env env, const float* values, size_t count) {
  auto array = Napi::Float32Array::New(env, count);
  if (count > 0 && values != nullptr) {
    std::memcpy(array.Data(), values, count * sizeof(float));
  }
  return array;
}

/// @brief Reads an edit off its own JS object. A zeroed SonareNoteEdit is the
///        identity, so an omitted key is a no-op.
///
/// The envelope is appended to @p envelopes and addressed by offset, because that
/// is how the by-value C ABI keeps the pool's ownership straight; a handle-based
/// door ignores both fields and reads the points out of @p envelopes itself.
inline void ReadNoteEditFields(const Napi::Object& edit, std::vector<float>* envelopes,
                               SonareNoteEdit* out) {
  out->time_offset_samples = node_int64_option(edit, "timeOffsetSamples", 0);
  out->pitch_shift_semitones = node_float_option(edit, "pitchShiftSemitones", 0.0f);
  out->gain_db = node_float_option(edit, "gainDb", 0.0f);
  out->time_stretch_ratio = node_float_option(edit, "timeStretchRatio", 0.0f);
  out->formant_shift_semitones = node_float_option(edit, "formantShiftSemitones", 0.0f);
  out->vibrato_depth_change = node_float_option(edit, "vibratoDepthChange", 0.0f);
  out->drift_change = node_float_option(edit, "driftChange", 0.0f);
  out->muted = node_bool_option(edit, "muted", false) ? 1 : 0;

  const std::vector<float> envelope = FloatArrayProperty(edit, "amplitudeEnvelope");
  out->envelope_offset = static_cast<int64_t>(envelopes->size());
  out->envelope_count = envelope.size();
  envelopes->insert(envelopes->end(), envelope.begin(), envelope.end());
}

/// @brief Reads a note's optional `edit` key through @ref ReadNoteEditFields.
///        An absent edit leaves @p out untouched, which is the identity.
inline void ReadNoteEdit(const char* fn, const Napi::Object& note, std::vector<float>* envelopes,
                         SonareNoteEdit* out) {
  const Napi::Value edit_value = note.Get("edit");
  if (edit_value.IsUndefined() || edit_value.IsNull()) {
    return;
  }
  if (!edit_value.IsObject() || edit_value.IsArray()) {
    throw std::runtime_error(std::string(fn) + ": note.edit must be a plain object");
  }
  ReadNoteEditFields(edit_value.As<Napi::Object>(), envelopes, out);
}

/// @brief Marshals one edit, with its own copy of its envelope points, so the
///        pool's offsets never reach JS.
inline Napi::Object NoteEditToJs(Napi::Env env, const SonareNoteEdit& edit, const float* envelope,
                                 size_t envelope_count) {
  Napi::Object out = Napi::Object::New(env);
  // No int64 in N-API: sample positions marshal as JS numbers, exact up to
  // Number.MAX_SAFE_INTEGER (2^53-1 samples, millennia of audio).
  out.Set("timeOffsetSamples",
          Napi::Number::New(env, static_cast<double>(edit.time_offset_samples)));
  out.Set("pitchShiftSemitones", Napi::Number::New(env, edit.pitch_shift_semitones));
  out.Set("gainDb", Napi::Number::New(env, edit.gain_db));
  out.Set("timeStretchRatio", Napi::Number::New(env, edit.time_stretch_ratio));
  out.Set("formantShiftSemitones", Napi::Number::New(env, edit.formant_shift_semitones));
  out.Set("vibratoDepthChange", Napi::Number::New(env, edit.vibrato_depth_change));
  out.Set("driftChange", Napi::Number::New(env, edit.drift_change));
  out.Set("muted", Napi::Boolean::New(env, edit.muted != 0));
  out.Set("amplitudeEnvelope", CopyToFloat32(env, envelope, envelope_count));
  return out;
}

/// @brief Marshals one note, with its own copies of the amplitude curve and the
///        edit's envelope points.
///
/// Both curves are passed in rather than derived, because the two doors hold them
/// differently: the by-value result slices shared pools, and the handle answers
/// per note through its own accessors.
inline Napi::Object NoteObjectToJs(Napi::Env env, const SonareNoteObject& note,
                                   const float* amplitude, size_t amplitude_count,
                                   const float* envelope, size_t envelope_count) {
  Napi::Object row = Napi::Object::New(env);
  row.Set("onsetSample", Napi::Number::New(env, static_cast<double>(note.onset_sample)));
  row.Set("offsetSample", Napi::Number::New(env, static_cast<double>(note.offset_sample)));
  row.Set("frameStart", Napi::Number::New(env, note.frame_start));
  row.Set("frameEnd", Napi::Number::New(env, note.frame_end));
  row.Set("medianHz", Napi::Number::New(env, note.median_hz));
  row.Set("medianCents", Napi::Number::New(env, note.median_cents));
  row.Set("f0Stability", Napi::Number::New(env, note.f0_stability));
  row.Set("edit", NoteEditToJs(env, note.edit, envelope, envelope_count));
  row.Set("amplitude", CopyToFloat32(env, amplitude, amplitude_count));
  return row;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_NOTE_OBJECTS_H_
