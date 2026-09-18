#pragma once

/// @file note_target.h
/// @brief Assigning a reference melody's pitches to a set of note objects.
///
/// The reference is a list of intervals in seconds, each carrying the pitch that
/// stretch of the part is supposed to be. Assignment writes each note's
/// pitch_shift_semitones from the target it overlaps, which is what makes a
/// take follow a written melody instead of a single stated interval.
///
/// The rule lives here and only here. It is a handful of arithmetic, which is
/// exactly why every surface would otherwise reimplement it and drift: how much
/// overlap counts, which of two overlapping targets wins, what happens to a note
/// no target reaches, and whether the correction saturates or is refused are all
/// decisions, not derivations.

#include <cstddef>
#include <vector>

#include "editing/note_model/note_object.h"

namespace sonare::editing::note_model {

/// One note of the reference melody. Times are seconds from the start of the
/// audio the notes were extracted from.
struct NoteTarget {
  double start_sec = 0.0;
  double end_sec = 0.0;
  float target_midi = 0.0f;
};

/// What to do with a note that has a measurable pitch but no target.
enum class UnmatchedTargetPolicy {
  /// Leave the edit alone; the note renders as it was recorded.
  Leave = 0,
  /// Mute the note's span.
  Mute = 1,
  /// Take the target nearest in time, however far away it is.
  Nearest = 2,
};

struct NoteTargetAssignConfig {
  UnmatchedTargetPolicy unmatched_policy = UnmatchedTargetPolicy::Leave;
  /// Fraction of the note that must overlap a target for it to count.
  float min_overlap_ratio = 0.5f;
  /// The assigned shift saturates here rather than being refused, matching
  /// formant_shift_semitones: a reference an octave out is a wrong reference,
  /// and a rejected call tells the caller less than a bounded correction does.
  float max_correction_semitones = 12.0f;
};

/// @brief Writes each note's pitch_shift_semitones from the target it overlaps.
/// @details Rewrites @p notes in place, so `edit` means the same field going in
///          and coming out and this introduces no second result type.
///
///          A note is matched to the target it overlaps longest, provided that
///          overlap is at least @c min_overlap_ratio of the note's own span;
///          ties go to the target that starts first. The shift is
///          `target_midi - midi_of(median_hz)`, saturated at
///          @c max_correction_semitones.
///
///          **A note whose @c median_hz is not finite and positive is never
///          assigned and never edited, whatever the policy says.** Such a note
///          has no measured pitch to correct from -- the extractor spells that
///          the way an F0 track spells an unvoiced frame -- so `Nearest` would
///          compute a shift from a pitch that does not exist. The policy governs
///          notes that have a pitch and no target, which is a different thing
///          from having no pitch at all.
/// @param sample_rate Converts each note's sample span to seconds; must be > 0.
/// @return How many notes received a target. Zero is a legitimate answer and the
///         caller has to be able to see it, which is why it is returned rather
///         than left implicit in the notes.
size_t assign_note_targets(std::vector<NoteObject>& notes, int sample_rate,
                           const std::vector<NoteTarget>& targets,
                           const NoteTargetAssignConfig& config);

}  // namespace sonare::editing::note_model
