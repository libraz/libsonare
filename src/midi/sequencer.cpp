#include "midi/sequencer.h"

#include <memory>
#include <utility>

#include "midi/ump.h"

namespace sonare::midi {

void MidiSequencer::prepare(double sample_rate) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  reset();
}

void MidiSequencer::reset() noexcept {
  active_count_ = 0;
  dispatched_event_count_.store(0, std::memory_order_relaxed);
}

void MidiSequencer::set_midi_clips(std::vector<MidiClipSchedule> clips) {
  const size_t count = clips.size();
  auto snapshot = std::make_shared<const std::vector<MidiClipSchedule>>(std::move(clips));
  if (clips_.publish(std::move(snapshot))) {
    clip_count_.store(count, std::memory_order_relaxed);
  }
}

bool MidiSequencer::track_note_on(uint8_t group, uint8_t channel, uint8_t note,
                                  uint32_t destination_id) noexcept {
  if (active_count_ >= kMaxActiveNotes) {
    active_note_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  active_[active_count_] = ActiveNote{group, channel, note, destination_id};
  ++active_count_;
  return true;
}

void MidiSequencer::track_note_off(uint8_t group, uint8_t channel, uint8_t note) noexcept {
  for (size_t i = 0; i < active_count_; ++i) {
    if (active_[i].group == group && active_[i].channel == channel && active_[i].note == note) {
      // Swap-remove (order of sounding notes is not significant).
      active_[i] = active_[active_count_ - 1];
      --active_count_;
      return;
    }
  }
}

void MidiSequencer::dispatch(uint32_t destination_id, const MidiEvent& event) noexcept {
  dispatched_event_count_.fetch_add(1, std::memory_order_relaxed);
  if (sink_ != nullptr) {
    sink_->on_event(destination_id, event);
  }
}

void MidiSequencer::process_block(int64_t block_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t block_end_frame = block_start_frame + num_frames;
  const std::vector<MidiClipSchedule>* clips = clips_.current();
  if (clips == nullptr) return;

  for (const MidiClipSchedule& clip : *clips) {
    // Events are sorted ascending by render_frame; scan the in-block window.
    for (const MidiEvent& ev : clip.events) {
      if (ev.render_frame < block_start_frame) continue;
      if (ev.render_frame >= block_end_frame) break;
      // Maintain the active-note table for hang-note safety.
      if (ev.ump.is_note_on()) {
        track_note_on(ev.ump.group, ev.ump.channel(), ev.ump.note_number(), clip.destination_id);
      } else if (ev.ump.is_note_off()) {
        track_note_off(ev.ump.group, ev.ump.channel(), ev.ump.note_number());
      }
      dispatch(clip.destination_id, ev);
    }
  }
}

void MidiSequencer::all_notes_off(int64_t render_frame) noexcept {
  // Emit a note-off for every sounding note, then clear the table. Iterate a
  // snapshot of the count because dispatch() does not mutate active_, and we
  // clear at the end; no allocation.
  for (size_t i = 0; i < active_count_; ++i) {
    const ActiveNote& note = active_[i];
    MidiEvent off;
    off.render_frame = render_frame;
    off.ump = make_midi1_note_off(note.group, note.channel, note.note, 0);
    dispatch(note.destination_id, off);
  }
  active_count_ = 0;
}

void MidiSequencer::inject_event(uint32_t destination_id, int64_t render_frame,
                                 const Ump& ump) noexcept {
  // Mirror process_block's active-note bookkeeping so a live note-on/off keeps
  // the hang-note table consistent, then dispatch at the requested render frame.
  if (ump.is_note_on()) {
    track_note_on(ump.group, ump.channel(), ump.note_number(), destination_id);
  } else if (ump.is_note_off()) {
    track_note_off(ump.group, ump.channel(), ump.note_number());
  }
  MidiEvent event;
  event.render_frame = render_frame;
  event.ump = ump;
  dispatch(destination_id, event);
}

void MidiSequencer::collect_boundaries(int64_t block_start_frame, int num_frames,
                                       BoundaryOffsets* out) const noexcept {
  if (out == nullptr) return;
  out->size = 0;
  out->overflowed = false;
  if (num_frames <= 0) return;
  const int64_t block_end_frame = block_start_frame + num_frames;
  const std::vector<MidiClipSchedule>* clips = clips_.current();
  if (clips == nullptr) return;

  for (const MidiClipSchedule& clip : *clips) {
    for (const MidiEvent& ev : clip.events) {
      if (ev.render_frame < block_start_frame) continue;
      if (ev.render_frame >= block_end_frame) break;
      const int offset = static_cast<int>(ev.render_frame - block_start_frame);
      if (out->size >= BoundaryOffsets::kCapacity) {
        out->overflowed = true;
        continue;
      }
      // De-dup adjacent identical offsets cheaply (events are sorted).
      if (out->size > 0 && out->offsets[out->size - 1] == offset) continue;
      out->offsets[out->size++] = offset;
    }
  }
}

}  // namespace sonare::midi
