#include "midi/note_targets.h"

#include <algorithm>

#include "midi/midi_clip.h"
#include "midi/note_pairing.h"
#include "midi/smf.h"
#include "transport/tempo_map.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::midi {

namespace {

using constants::kDefaultDawSampleRate;

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
    throw SonareException(ErrorCode::InvalidParameter, "bytes is NULL");
  }
  const SmfImportResult imported = import_smf(bytes, size);
  if (!imported.recoverable()) {
    throw SonareException(ErrorCode::InvalidFormat, "unreadable SMF");
  }
  if (imported.clips.empty()) {
    util::check::refuse("track_index", "must select a MIDI-bearing track, but the file has none");
  }
  const auto last_track_index = static_cast<long long>(imported.clips.size() - 1);
  util::check::integer_in_range(track_index, 0, last_track_index, "track_index");

  transport::TempoMap map;
  load_tempo_map(map, imported);
  const auto seconds_at = [&map](double ppq) {
    return static_cast<double>(map.ppq_to_sample(ppq)) / static_cast<double>(kDefaultDawSampleRate);
  };

  // A copy, because the sort is what makes the LIFO note-off policy operate on
  // musical time rather than on the order in which the file stored events.
  MidiClip clip = imported.clips[static_cast<size_t>(track_index)];
  clip.sort_stable();

  std::vector<editing::note_model::NoteTarget> targets;
  const auto decode = [](const MidiClipEvent& event) {
    return detail::DecodedNoteEvent{event.ppq, event.ump};
  };
  const auto emit = [&targets, &seconds_at](const detail::NotePair& note) {
    editing::note_model::NoteTarget target;
    target.start_sec = seconds_at(note.start_ppq);
    target.end_sec = seconds_at(note.end_ppq);
    target.target_midi = static_cast<float>(note.note);
    // A zero-length note overlaps nothing, so it could never be assigned.
    if (target.end_sec > target.start_sec) targets.push_back(target);
  };
  detail::pair_notes(clip.events(), decode, emit);

  // Whatever is still open has no end, and the track's end is not one -- see the
  // header for why substituting it is worse than dropping the note.
  std::sort(targets.begin(), targets.end(),
            [](const auto& left, const auto& right) { return left.start_sec < right.start_sec; });
  return targets;
}

}  // namespace sonare::midi
