#pragma once

/// @file note_val.h
/// @brief JS <-> note_model::NoteObject / NoteEdit conversion, shared by the WASM
///        note-object and polyphonic-editing TUs.
///
/// An absent edit field is its own identity spelling, so `{}` and an omitted edit
/// both leave the note untouched, and a `timeStretchRatio` of 0 reads as 1 -- the C
/// ABI's rule, kept so one edit means one thing whichever chain applies it.
///
/// Two things the C ABI's fixed records cannot carry inline cross as arrays here,
/// because nothing on this surface needs a pool to own them: a note's amplitude
/// curve, and an edit's envelope points. The @c amplitude_offset and
/// @c envelope_offset that index that pool therefore have no counterpart.

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "editing/note_model/note_object.h"
#include "wasm/bindings/common/common.h"

namespace sonare_wasm_editing {

/// @brief One edit's own amplitude envelope, budgeted before it is copied.
inline std::vector<float> noteEnvelopeFromVal(const val& edit, const char* budget_subject,
                                              std::size_t* cumulative_count) {
  const val envelope = objectProperty(edit, "amplitudeEnvelope");
  if (envelope.isUndefined()) return {};
  accumulateWasmFloat32ArrayLength(envelope, "amplitudeEnvelope", budget_subject, cumulative_count);
  return float32ArrayToVector(envelope);
}

/// @brief Reads a JS edit object.
/// @param subject Names the edit in a rejection, e.g. "setNoteEdit edit".
/// @param budget_subject Names the entry point's cumulative input budget.
inline sonare::editing::note_model::NoteEdit noteEditFromVal(const val& edit, const char* subject,
                                                             const char* budget_subject,
                                                             std::size_t* cumulative_count) {
  sonare::editing::note_model::NoteEdit out;
  if (hasProperty(edit, "timeOffsetSamples")) {
    out.time_offset_samples =
        static_cast<int64_t>(requireNumberProperty(edit, "timeOffsetSamples", subject));
  }
  out.pitch_shift_semitones = floatProperty(edit, "pitchShiftSemitones", 0.0f);
  out.gain_db = floatProperty(edit, "gainDb", 0.0f);
  const float stretch_ratio = floatProperty(edit, "timeStretchRatio", 0.0f);
  // A zeroed edit must be the identity, so 0 reads as 1 (SonareNoteEdit).
  out.time_stretch_ratio = stretch_ratio == 0.0f ? 1.0f : stretch_ratio;
  out.formant_shift_semitones = floatProperty(edit, "formantShiftSemitones", 0.0f);
  out.vibrato_depth_change = floatProperty(edit, "vibratoDepthChange", 0.0f);
  out.drift_change = floatProperty(edit, "driftChange", 0.0f);
  out.muted = boolProperty(edit, "muted", false);
  out.amplitude_envelope = noteEnvelopeFromVal(edit, budget_subject, cumulative_count);
  return out;
}

/// @brief Reads the edit riding on a JS note object, which is where the by-value
///        door carries it.
/// @param entry_point Names the call in a rejection, e.g. "renderNotes".
inline sonare::editing::note_model::NoteEdit noteRowEditFromVal(const val& row,
                                                                const char* entry_point,
                                                                std::size_t* cumulative_count) {
  const std::string subject = std::string(entry_point) + " note.edit";
  const std::string budget = std::string(entry_point) + " input";
  return noteEditFromVal(objectProperty(row, "edit"), subject.c_str(), budget.c_str(),
                         cumulative_count);
}

/// @brief Writes one edit back, field for field.
inline val noteEditToVal(const sonare::editing::note_model::NoteEdit& edit) {
  val out = val::object();
  // Sample positions cross as plain JS numbers rather than BigInt, matching every
  // other int64 field on this surface.
  out.set("timeOffsetSamples", static_cast<double>(edit.time_offset_samples));
  out.set("pitchShiftSemitones", edit.pitch_shift_semitones);
  out.set("gainDb", edit.gain_db);
  out.set("timeStretchRatio", edit.time_stretch_ratio);
  out.set("formantShiftSemitones", edit.formant_shift_semitones);
  out.set("vibratoDepthChange", edit.vibrato_depth_change);
  out.set("driftChange", edit.drift_change);
  out.set("muted", edit.muted);
  out.set("amplitudeEnvelope", vectorToFloat32Array(edit.amplitude_envelope));
  return out;
}

/// @brief One note as a JS object.
/// @details Field names and defaults mirror the C ABI (sonare_extract_notes /
///          sonare_render_notes) with one deliberate difference: the per-note F0
///          curve is not surfaced. Through the by-value door it is the caller's own
///          @c f0Hz sliced by [frameStart, frameEnd); through a handle it has its
///          own accessor.
inline val noteObjectToVal(const sonare::editing::note_model::NoteObject& note) {
  val row = val::object();
  row.set("onsetSample", static_cast<double>(note.onset_sample));
  row.set("offsetSample", static_cast<double>(note.offset_sample));
  row.set("frameStart", note.frame_start);
  row.set("frameEnd", note.frame_end);
  row.set("medianHz", note.median_hz);
  row.set("medianCents", note.median_cents);
  row.set("f0Stability", note.f0_stability);
  row.set("amplitude", vectorToFloat32Array(note.amplitude.values));
  row.set("edit", noteEditToVal(note.edit));
  return row;
}

inline val noteObjectsToVal(const std::vector<sonare::editing::note_model::NoteObject>& notes) {
  val out = val::array();
  for (const sonare::editing::note_model::NoteObject& note : notes) {
    out.call<void>("push", noteObjectToVal(note));
  }
  return out;
}

}  // namespace sonare_wasm_editing

#endif  // __EMSCRIPTEN__
