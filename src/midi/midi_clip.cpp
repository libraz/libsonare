#include "midi/midi_clip.h"

#include <algorithm>
#include <array>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sonare::midi {

namespace {

using PreparedOwners =
    std::unordered_map<const PreparedMidiSysEx*, std::shared_ptr<const PreparedMidiSysEx>>;

// Indexes every prepared token owned by a distinct source bank, so retaining a
// token costs one lookup instead of a scan over every schedule's bank.
PreparedOwners index_prepared_owners(const std::vector<MidiClipSchedule>& schedules) {
  PreparedOwners owners;
  std::unordered_set<const MidiSysExPayloadBank*> seen_banks;
  for (const MidiClipSchedule& schedule : schedules) {
    const MidiSysExPayloadBank* bank = schedule.sysex_payload_bank.get();
    if (bank == nullptr || !seen_banks.insert(bank).second) continue;
    for (const std::shared_ptr<const PreparedMidiSysEx>& prepared : bank->prepared_operations) {
      if (prepared != nullptr) owners.emplace(prepared.get(), prepared);
    }
  }
  return owners;
}

}  // namespace

// A deterministic ordering rank for events sharing the same timestamp. Note-off
// must precede note-on so a same-timestamp re-trigger releases before
// re-attacking (no stuck/dropped note). Other messages fall in between by status
// nibble.
int same_time_rank(const Ump& ump) noexcept {
  if (ump.is_note_off()) return 0;
  const auto message_type = ump.message_type();
  if (message_type == UmpMessageType::kMidi1ChannelVoice) {
    const auto status = static_cast<UmpStatus>(ump.status_nibble());
    if (status == UmpStatus::kControlChange) {
      const uint8_t controller = ump.note_number();
      if (controller == 0) return 1;
      if (controller == 32) return 2;
      return kGeneralRank;
    }
    if (status == UmpStatus::kProgramChange) return 3;
  } else if (message_type == UmpMessageType::kMidi2ChannelVoice) {
    // MIDI 2.0 carries bank select inside the program-change message itself, so
    // there is no separate bank-select CC to order ahead of it; rank the banked
    // PC before note-on exactly like the MIDI 1.0 program change, so a note at
    // the same timestamp uses the new program/bank.
    if (static_cast<UmpStatus>(ump.status_nibble()) == UmpStatus::kProgramChange) return 3;
  }
  if (ump.is_note_on()) return kMaxSameTimeRank;
  return kGeneralRank;
}

namespace {

bool same_timestamp_ump_before(const Ump& a, const Ump& b) noexcept {
  const int ra = same_time_rank(a);
  const int rb = same_time_rank(b);
  if (ra != rb) return ra < rb;
  // Two events sharing kGeneralRank are a gesture, not a set: leave them in
  // stream order. See MidiClip::sort_stable for why.
  if (ra == kGeneralRank) return false;
  // Deterministic tiebreak on note then channel then first word so identical
  // timestamps are fully ordered regardless of insertion order. Mirrors
  // MidiClip::sort_stable.
  if (a.note_number() != b.note_number()) return a.note_number() < b.note_number();
  if (a.channel() != b.channel()) return a.channel() < b.channel();
  return a.words[0] < b.words[0];
}

}  // namespace

bool render_event_before(const MidiEvent& a, const MidiEvent& b) noexcept {
  if (a.render_frame != b.render_frame) return a.render_frame < b.render_frame;
  return same_timestamp_ump_before(a.ump, b.ump);
}

void sort_render_events_stable(std::vector<MidiEvent>& events) {
  std::stable_sort(events.begin(), events.end(), render_event_before);
}

void sort_render_events_stable(MidiEvent* events, size_t count) {
  if (events == nullptr || count < 2) return;
  // Binary insertion sort: stable and fully in place. std::stable_sort requests
  // a temporary heap buffer (operator new), which is forbidden on the audio
  // thread -- this overload exists precisely for the fixed-capacity MidiFxBuffer
  // that process() sorts per block. Event counts here are small (a few dozen,
  // capped at MidiFxBuffer::kCapacity), so the O(n^2) moves are negligible.
  for (size_t i = 1; i < count; ++i) {
    MidiEvent key = events[i];
    // upper_bound keeps equal-key elements in their original relative order
    // (stability), matching render_event_before's deterministic tiebreak.
    size_t lo = 0;
    size_t hi = i;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (render_event_before(key, events[mid])) {
        hi = mid;
      } else {
        lo = mid + 1;
      }
    }
    for (size_t j = i; j > lo; --j) {
      events[j] = events[j - 1];
    }
    events[lo] = key;
  }
}

void MidiClip::set_events(std::vector<MidiClipEvent> events) { events_ = std::move(events); }

void MidiClip::add_event(const MidiClipEvent& event) { events_.push_back(event); }

void MidiClip::sort_stable() {
  std::stable_sort(events_.begin(), events_.end(),
                   [](const MidiClipEvent& a, const MidiClipEvent& b) {
                     if (a.ppq != b.ppq) return a.ppq < b.ppq;
                     return same_timestamp_ump_before(a.ump, b.ump);
                   });
}

NotePairValidation MidiClip::validate_note_pairs() const {
  NotePairValidation result;
  // Count of currently-open note-ons per (group, channel, note). UMP group is
  // part of the MIDI endpoint identity and must not match across groups.
  std::array<std::array<std::array<int, 128>, 16>, 16> open{};

  for (const MidiClipEvent& ev : events_) {
    const uint8_t group = ev.ump.group;
    const uint8_t channel = ev.ump.channel();
    const uint8_t note = ev.ump.note_number();
    if (group >= 16 || channel >= 16 || note >= 128) continue;
    if (ev.ump.is_note_on()) {
      open[group][channel][note]++;
    } else if (ev.ump.is_note_off()) {
      if (open[group][channel][note] > 0) {
        open[group][channel][note]--;
      } else {
        result.unmatched_note_offs++;
      }
    }
  }
  for (const auto& group : open) {
    for (const auto& ch : group) {
      for (int count : ch) {
        if (count > 0) result.unmatched_note_ons += static_cast<uint32_t>(count);
      }
    }
  }
  result.ok = result.unmatched_note_ons == 0 && result.unmatched_note_offs == 0;
  return result;
}

void MidiClip::to_render_events(const transport::TempoMap& tempo_map, double clip_start_ppq,
                                std::vector<MidiEvent>* out) const {
  if (out == nullptr) return;
  for (const MidiClipEvent& ev : events_) {
    MidiEvent rendered;
    // Absolute mapping, as the header states: each event's own position on the
    // timeline, mapped through the tempo map in one step. The clip start is a
    // term in that sum, not a separate anchor -- the sequencer recomputes the
    // local offset it needs from the clip's own start_sample, so subtracting one
    // here would only reintroduce the rounding it already accounts for.
    rendered.render_frame = tempo_map.ppq_to_sample(clip_start_ppq + ev.ppq);
    rendered.ump = ev.ump;
    rendered.sysex_payload = ev.sysex_payload;
    rendered.sysex_payload_size = ev.sysex_payload_size;
    out->push_back(rendered);
  }
}

const char* describe(MidiSysExPayloadError error) noexcept {
  switch (error) {
    case MidiSysExPayloadError::kNone:
      return "no MIDI SysEx payload error";
    case MidiSysExPayloadError::kNullPayload:
      return "MIDI SysEx payload has a null pointer with nonzero size";
    case MidiSysExPayloadError::kPreparationFailed:
      return "MIDI SysEx preparation was refused by the destination instrument";
    case MidiSysExPayloadError::kOutOfMemory:
      return "MIDI SysEx payload allocation failed";
  }
  return "unknown MIDI SysEx payload error";
}

bool own_sysex_payloads(std::vector<MidiClipSchedule>& schedules, MidiSysExPayloadError* error,
                        const MidiSysExPreparer& prepare) {
  if (error != nullptr) *error = MidiSysExPayloadError::kNone;

  size_t payload_count = 0;
  for (const MidiClipSchedule& schedule : schedules) {
    for (const MidiEvent& event : schedule.events) {
      if (!is_sysex_event(event)) continue;
      if (event.sysex_payload_size == 0) continue;
      if (event.sysex_payload == nullptr) {
        if (error != nullptr) *error = MidiSysExPayloadError::kNullPayload;
        return false;
      }
      ++payload_count;
    }
  }

  if (payload_count == 0) {
    for (MidiClipSchedule& schedule : schedules) {
      schedule.sysex_payload_bank.reset();
      for (MidiEvent& event : schedule.events) {
        event.sysex_payload = nullptr;
        event.sysex_payload_size = 0;
        event.prepared_sysex = nullptr;
      }
    }
    return true;
  }

  // A later event may borrow from an earlier source bank, so mutate nothing yet.
  try {
    PreparedOwners owners;
    if (!prepare) owners = index_prepared_owners(schedules);
    auto bank = std::make_shared<MidiSysExPayloadBank>();
    bank->payloads.reserve(payload_count);
    bank->prepared_operations.reserve(payload_count);
    for (const MidiClipSchedule& schedule : schedules) {
      for (const MidiEvent& event : schedule.events) {
        if (!is_sysex_event(event) || event.sysex_payload_size == 0) continue;
        bank->payloads.emplace_back(event.sysex_payload,
                                    event.sysex_payload + event.sysex_payload_size);

        std::shared_ptr<const PreparedMidiSysEx> prepared;
        if (prepare) {
          if (!prepare(schedule.destination_id, event.sysex_payload, event.sysex_payload_size,
                       prepared)) {
            if (error != nullptr) *error = MidiSysExPayloadError::kPreparationFailed;
            return false;
          }
        } else if (event.prepared_sysex != nullptr) {
          // A raw pointer does not establish lifetime; keep only a bank-owned token.
          const auto owner = owners.find(event.prepared_sysex);
          if (owner != owners.end()) prepared = owner->second;
        }
        bank->prepared_operations.push_back(std::move(prepared));
      }
    }

    // Retarget only after every source span and prepared token has been copied.
    size_t payload_index = 0;
    for (MidiClipSchedule& schedule : schedules) {
      for (MidiEvent& event : schedule.events) {
        if (!is_sysex_event(event) || event.sysex_payload_size == 0) {
          event.sysex_payload = nullptr;
          event.sysex_payload_size = 0;
          event.prepared_sysex = nullptr;
          continue;
        }
        event.sysex_payload = bank->payloads[payload_index].data();
        event.prepared_sysex = bank->prepared_operations[payload_index].get();
        ++payload_index;
      }
    }
    for (MidiClipSchedule& schedule : schedules) schedule.sysex_payload_bank = bank;
    return true;
  } catch (const std::bad_alloc&) {
    if (error != nullptr) *error = MidiSysExPayloadError::kOutOfMemory;
    return false;
  } catch (...) {
    if (error != nullptr) *error = MidiSysExPayloadError::kPreparationFailed;
    return false;
  }
}

}  // namespace sonare::midi
