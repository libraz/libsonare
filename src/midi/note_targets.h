#pragma once

/// @file note_targets.h
/// @brief Reading one track of a Standard MIDI File as a reference melody.
///
/// The note model's assignment rule takes targets in seconds, and an SMF times
/// its events in quarter notes. The conversion is not a scale factor: a file
/// may change tempo, and may ramp it, so it goes through the tempo map the
/// importer recovered rather than through the file's initial tempo.
///
/// This lives in the MIDI library rather than beside the rule it feeds, because
/// the core does not link the MIDI library -- the dependency runs the other way,
/// and putting an SMF reader in the core would close a cycle.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "editing/note_model/note_target.h"

namespace sonare::midi {

/// @brief Reads @p track_index of an in-memory SMF as reference note targets.
/// @details Each note-on is paired with the next note-off of the same note
///          number on the same channel, and the pair becomes one target at the
///          note's own pitch. **A note-on the track never closes is dropped.** It
///          has no end, and the track's end is not a substitute for one: the
///          importer derives that from the last event, which is the open note-on
///          itself when the file stops there, so the note would come back with
///          zero length. Worse in the other case -- an open note-on with events
///          after it would span the whole remainder, and being the longest
///          overlap it would then win the assignment for every note that
///          follows, so one stuck note-on would repitch the rest of the take.
///
///          Times come from the importer's tempo map through
///          @c transport::TempoMap, so a tempo change or a tempo ramp inside the
///          file is followed. The conversion passes through samples at 48 kHz,
///          which places a boundary within one sample of its true time -- four
///          orders of magnitude below anything a note boundary means.
///
///          Zero-length notes are skipped: they cannot overlap anything, so they
///          would occupy a target slot that can never be assigned.
/// @param track_index Index into the tracks that carried MIDI events, NOT the
///        SMF's own track numbering: a track holding only meta events -- a
///        conductor track carrying the tempo map is the usual one -- produces no
///        clip and is not counted here. A file whose first track is a conductor
///        track therefore has its melody at index 0, not 1.
/// @throws sonare::SonareException(InvalidFormat) — the bytes are not a readable
///         SMF.
/// @throws sonare::SonareException(InvalidParameter) — @p bytes is NULL with a
///         non-zero @p size, or @p track_index names no MIDI-bearing track.
std::vector<editing::note_model::NoteTarget> note_targets_from_smf(const uint8_t* bytes,
                                                                   size_t size, int track_index);

}  // namespace sonare::midi
