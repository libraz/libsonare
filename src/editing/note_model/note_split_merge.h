#pragma once

/// @file note_split_merge.h
/// @brief Cutting one note in two and joining a run of notes into one.
///
/// Both re-derive the resulting notes from the source the way the extractor
/// does, rather than patching the fields of the notes they replace: a merge
/// spans the unvoiced frames the segmenter cut at, and those frames' pitch and
/// amplitude live in the track and the audio, not in either neighbour.
///
/// Both return a new list and leave the input alone, like every other edit in
/// this model.

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/pitch_editor/f0_provider.h"

namespace sonare::editing::note_model {

/// @brief Splits @p notes[index] at @p frame.
/// @details Both halves inherit the source note's edit, and its amplitude
///          envelope is cut at the same proportion so each half keeps its own
///          part of it. A note whose edit is the identity therefore still
///          renders bit for bit after being split.
///
///          A one-entry envelope is a constant over the span, so both halves
///          get that same one entry and the constant survives exactly. A longer
///          one is read endpoint-anchored, the way the renderer resamples it,
///          and both halves carry the value at the cut. That reproduces the
///          shape rather than the samples: each half now anchors its own
///          endpoints, so the rendered gain near the seam moves by about one
///          envelope step.
/// @param frame Track frame to cut at, strictly inside the note's own span.
/// @throws SonareException(InvalidParameter) on an out-of-range @p index, a
///         @p frame outside (frame_start, frame_end), or the inputs
///         @ref extract_notes itself rejects.
std::vector<NoteObject> split_note(const Audio& audio, const pitch_editor::F0Track& track,
                                   const std::vector<NoteObject>& notes, size_t index, int frame,
                                   const NoteExtractorConfig& config = {});

/// @brief Joins @p notes[first] through @p notes[last] into one note.
/// @details The result spans from the first note's onset to the last note's
///          offset, including whatever the segmenter cut out between them, and
///          its measured fields are derived over that whole span.
///
///          It takes @p notes[first]'s edit. Notes carrying different edits
///          have no single correct answer here, so the rule is stated rather
///          than guessed at -- a host that cares sets the edit afterwards.
/// @throws SonareException(InvalidParameter) unless
///         @c first < last < notes.size(), or on the inputs
///         @ref extract_notes itself rejects.
std::vector<NoteObject> merge_notes(const Audio& audio, const pitch_editor::F0Track& track,
                                    const std::vector<NoteObject>& notes, size_t first, size_t last,
                                    const NoteExtractorConfig& config = {});

}  // namespace sonare::editing::note_model
