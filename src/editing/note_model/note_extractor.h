#pragma once

/// @file note_extractor.h
/// @brief Builds note objects from audio and an F0 track.

#include <cstdint>
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
///          A missing sample rate uses the audio's rate; a stated rate must
///          match it. Samples per frame must be finite and in [1, INT_MAX].
///          Returned F0 curves encode unvoiced or unusable measurements as zero.
/// @throws SonareException(InvalidParameter) on empty audio, an empty track, a
///         track whose frame cadence is not positive, a selected voicing array
///         whose length does not match the track's frames,
///         a rate mismatch, samples per frame outside [1, INT_MAX], or a
///         non-finite config value.
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

/// @brief @ref make_note over a part of the input rather than all of it.
/// @details @p segment holds the input's samples from @p segment_start on, and
///          @p n_samples is the whole input's length, so the note comes back in
///          the whole input's positions. The segment has to cover the samples
///          the span's frames read; the rest of the input is never consulted.
/// @throws SonareException(InvalidParameter) on what @ref make_note rejects, a
///         negative @p segment_start, a non-positive @p n_samples, or a segment
///         that does not cover the span.
NoteObject make_note(const Audio& segment, int64_t segment_start, int64_t n_samples,
                     const pitch_editor::F0Track& track, int frame_start, int frame_end,
                     const NoteExtractorConfig& config = {});

/// @brief Repairs physical bounds after a note set was re-derived from frame spans.
/// @details A terminal frame whose raw sample range lies beyond the audio is
///        nudged one sample left by @ref make_note. When that frame follows a
///        re-derived note ending at the same physical boundary, the predecessor
///        is shortened to the nudge so the set remains disjoint. The note count
///        is preserved; a re-derived set that cannot keep every note positive is
///        rejected instead of dropping an entry.
/// @throws SonareException(InvalidParameter) on invalid extractor inputs, a
///         non-positive re-derived sample span, or a terminal repair that would
///         make a predecessor non-positive.
void repair_rederived_note_set_bounds(const Audio& audio, const pitch_editor::F0Track& track,
                                      std::vector<NoteObject>& notes,
                                      const NoteExtractorConfig& config = {});

}  // namespace sonare::editing::note_model
