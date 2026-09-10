#pragma once

/// @file note_extractor.h
/// @brief Builds note objects from audio and an F0 track.

#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_object.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_segmenter.h"

namespace sonare::editing::note_model {

struct NoteExtractorConfig {
  pitch_editor::NoteSegmenterConfig segmenter{};
  /// Voiced probability at or above which a frame counts as voiced, used only
  /// when the track carries no explicit voiced flags.
  float voiced_threshold = 0.5f;
};

/// @brief Extracts note objects from @p audio using @p track.
/// @details Spans come from NoteSegmenter. Each note then carries the F0 and
///          RMS curves over its own frames plus the two measured quality
///          figures. Every returned note has an identity edit.
///
///          Voicing comes from @c track.voiced; @c track.voiced_prob is read
///          only when that is empty, and its length is not checked otherwise.
///          A track stating only @c frame_rate_hz is served by the cadence rule
///          like any other -- it segments rather than coming back empty.
/// @throws SonareException(InvalidParameter) on empty audio, an empty track, a
///         track whose frame cadence is not positive, a voiced or voiced
///         probability array whose length does not match the track's frames, or
///         a non-finite config value.
std::vector<NoteObject> extract_notes(const Audio& audio, const pitch_editor::F0Track& track,
                                      const NoteExtractorConfig& config = {});

/// @brief Builds one note over @c [frame_start, frame_end) the way
///        @ref extract_notes builds each of its own.
/// @details The segmenter is not consulted -- the span is the caller's -- so
///          this is how a span that came from somewhere else (a split, a merge,
///          a host's own onset detector) gets the same measured fields as an
///          extracted one, from the same code. The returned note has an
///          identity edit.
/// @throws SonareException(InvalidParameter) on the inputs @ref extract_notes
///         rejects, or a span that is empty, reversed or outside the track.
NoteObject make_note(const Audio& audio, const pitch_editor::F0Track& track, int frame_start,
                     int frame_end, const NoteExtractorConfig& config = {});

}  // namespace sonare::editing::note_model
