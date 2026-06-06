#include "midi/midi_clip.h"

#include <algorithm>
#include <array>
#include <utility>

namespace sonare::midi {
namespace {

// A deterministic ordering rank for events sharing the same PPQ. Note-off must
// precede note-on so a same-timestamp re-trigger releases before re-attacking
// (no stuck note). Other messages fall in between by status nibble.
int same_time_rank(const Ump& ump) noexcept {
  if (ump.is_note_off()) return 0;
  if (ump.message_type() == UmpMessageType::kMidi1ChannelVoice) {
    const auto status = static_cast<UmpStatus>(ump.status_nibble());
    if (status == UmpStatus::kControlChange) {
      const uint8_t controller = ump.note_number();
      if (controller == 0) return 1;
      if (controller == 32) return 2;
      return 4;
    }
    if (status == UmpStatus::kProgramChange) return 3;
  }
  if (ump.is_note_on()) return 5;
  return 4;
}

}  // namespace

void MidiClip::set_events(std::vector<MidiClipEvent> events) { events_ = std::move(events); }

void MidiClip::add_event(const MidiClipEvent& event) { events_.push_back(event); }

void MidiClip::sort_stable() {
  std::stable_sort(events_.begin(), events_.end(),
                   [](const MidiClipEvent& a, const MidiClipEvent& b) {
                     if (a.ppq != b.ppq) return a.ppq < b.ppq;
                     const int ra = same_time_rank(a.ump);
                     const int rb = same_time_rank(b.ump);
                     if (ra != rb) return ra < rb;
                     // Stable tiebreak on note then channel then first word so
                     // identical-timestamp ordering is fully deterministic
                     // regardless of insertion order.
                     if (a.ump.note_number() != b.ump.note_number()) {
                       return a.ump.note_number() < b.ump.note_number();
                     }
                     if (a.ump.channel() != b.ump.channel()) {
                       return a.ump.channel() < b.ump.channel();
                     }
                     return a.ump.words[0] < b.ump.words[0];
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
  const int64_t clip_start_sample = tempo_map.ppq_to_sample(clip_start_ppq);
  for (const MidiClipEvent& ev : events_) {
    const int64_t abs_sample = tempo_map.ppq_to_sample(clip_start_ppq + ev.ppq);
    MidiEvent rendered;
    // Anchor relative to the clip start so any small PPQ->sample rounding stays
    // consistent with the clip's own start_sample baking in the compiler.
    rendered.render_frame = abs_sample;
    rendered.ump = ev.ump;
    rendered.sysex_payload = ev.sysex_payload;
    rendered.sysex_payload_size = ev.sysex_payload_size;
    (void)clip_start_sample;
    out->push_back(rendered);
  }
}

}  // namespace sonare::midi
