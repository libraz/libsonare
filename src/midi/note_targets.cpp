#include "midi/note_targets.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "midi/midi_clip.h"
#include "midi/smf.h"
#include "transport/tempo_map.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::midi {

namespace {

using constants::kDefaultDawSampleRate;

/// One note-on waiting for its note-off.
struct PendingNote {
  uint8_t note_number = 0;
  uint8_t channel = 0;
  double ppq = 0.0;
};

/// Loads @p map with the tempo the importer recovered. Filled in place because
/// TempoMap is not copyable.
void load_tempo_map(transport::TempoMap& map, const SmfImportResult& imported) {
  map.prepare(kDefaultDawSampleRate);
  map.set_segments(imported.tempo_segments);
  map.set_time_signatures(imported.time_signatures);
}

}  // namespace

std::vector<editing::note_model::NoteTarget> note_targets_from_smf(const uint8_t* bytes,
                                                                   size_t size, int track_index) {
  if (bytes == nullptr && size != 0) {
    throw SonareException(ErrorCode::InvalidParameter, "note_targets_from_smf: bytes is NULL");
  }
  const SmfImportResult imported = import_smf(bytes, size);
  if (!imported.recoverable()) {
    throw SonareException(ErrorCode::InvalidFormat, "note_targets_from_smf: unreadable SMF");
  }
  if (track_index < 0 || static_cast<size_t>(track_index) >= imported.clips.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "note_targets_from_smf: no MIDI-bearing track at index " +
                              std::to_string(track_index) + " (" +
                              std::to_string(imported.clips.size()) + " available)");
  }

  transport::TempoMap map;
  load_tempo_map(map, imported);
  const auto seconds_at = [&map](double ppq) {
    return static_cast<double>(map.ppq_to_sample(ppq)) / static_cast<double>(kDefaultDawSampleRate);
  };

  // A copy, because the sort is what makes "the next note-off" mean the next one
  // in time rather than the next one the file happened to store.
  MidiClip clip = imported.clips[static_cast<size_t>(track_index)];
  clip.sort_stable();

  std::vector<editing::note_model::NoteTarget> targets;
  std::vector<PendingNote> pending;
  const auto emit = [&targets, &seconds_at](const PendingNote& note, double end_ppq) {
    editing::note_model::NoteTarget target;
    target.start_sec = seconds_at(note.ppq);
    target.end_sec = seconds_at(end_ppq);
    target.target_midi = static_cast<float>(note.note_number);
    // A zero-length note overlaps nothing, so it could never be assigned.
    if (target.end_sec > target.start_sec) targets.push_back(target);
  };

  for (const MidiClipEvent& event : clip.events()) {
    if (event.ump.is_note_off()) {
      // Most recent first: a note retriggered before its first note-off closes
      // the newer sounding of it, which is what a sequencer does.
      const auto match = std::find_if(pending.rbegin(), pending.rend(), [&event](const auto& note) {
        return note.note_number == event.ump.note_number() && note.channel == event.ump.channel();
      });
      if (match == pending.rend()) continue;
      emit(*match, event.ppq);
      pending.erase(std::next(match).base());
      continue;
    }
    if (event.ump.is_note_on()) {
      pending.push_back({event.ump.note_number(), event.ump.channel(), event.ppq});
    }
  }

  // Whatever is still open has no end, and the track's end is not one -- see the
  // header for why substituting it is worse than dropping the note.
  std::sort(targets.begin(), targets.end(),
            [](const auto& left, const auto& right) { return left.start_sec < right.start_sec; });
  return targets;
}

}  // namespace sonare::midi
