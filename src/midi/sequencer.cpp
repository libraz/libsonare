#include "midi/sequencer.h"

#include <algorithm>
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
  pending_fx_count_ = 0;
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
                                  uint32_t destination_id, bool from_clip,
                                  uint32_t clip_id) noexcept {
  if (active_count_ >= kMaxActiveNotes) {
    active_note_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  active_[active_count_] = ActiveNote{group, channel, note, destination_id, clip_id, from_clip};
  ++active_count_;
  return true;
}

void MidiSequencer::track_note_off(uint8_t group, uint8_t channel, uint8_t note,
                                   uint32_t destination_id, bool from_clip,
                                   uint32_t clip_id) noexcept {
  size_t fallback = kMaxActiveNotes;
  for (size_t i = 0; i < active_count_; ++i) {
    if (active_[i].group == group && active_[i].channel == channel && active_[i].note == note &&
        active_[i].destination_id == destination_id) {
      if (active_[i].from_clip == from_clip && active_[i].clip_id == clip_id) {
        // Swap-remove (order of sounding notes is not significant).
        active_[i] = active_[active_count_ - 1];
        --active_count_;
        return;
      }
      if (fallback == kMaxActiveNotes) fallback = i;
    }
  }
  if (fallback != kMaxActiveNotes) {
    active_[fallback] = active_[active_count_ - 1];
    --active_count_;
  }
}

MidiSequencer::DestinationFx* MidiSequencer::find_midi_fx(uint32_t destination_id) noexcept {
  for (DestinationFx& fx : midi_fx_) {
    if (fx.active && fx.destination_id == destination_id) return &fx;
  }
  return nullptr;
}

const MidiSequencer::DestinationFx* MidiSequencer::find_midi_fx(
    uint32_t destination_id) const noexcept {
  for (const DestinationFx& fx : midi_fx_) {
    if (fx.active && fx.destination_id == destination_id) return &fx;
  }
  return nullptr;
}

bool MidiSequencer::set_midi_fx(uint32_t destination_id, const MidiFxChain& chain) noexcept {
  DestinationFx* slot = find_midi_fx(destination_id);
  if (slot == nullptr) {
    for (DestinationFx& candidate : midi_fx_) {
      if (!candidate.active) {
        slot = &candidate;
        slot->active = true;
        slot->destination_id = destination_id;
        break;
      }
    }
  }
  if (slot == nullptr) return false;
  slot->chain.set_transpose(chain.transpose());
  slot->chain.set_quantize(chain.quantize());
  slot->chain.set_velocity_curve(chain.velocity_curve());
  slot->chain.set_chord(chain.chord());
  slot->chain.set_arpeggiator(chain.arpeggiator());
  slot->chain.set_humanize(chain.humanize());
  slot->chain.prepare();
  slot->buffer.clear();
  return true;
}

void MidiSequencer::clear_midi_fx(uint32_t destination_id) noexcept {
  DestinationFx* slot = find_midi_fx(destination_id);
  if (slot == nullptr) return;
  slot->active = false;
  slot->destination_id = 0;
  slot->buffer.clear();
  clear_pending_for_destination(destination_id);
}

void MidiSequencer::dispatch(uint32_t destination_id, const MidiEvent& event) noexcept {
  dispatched_event_count_.fetch_add(1, std::memory_order_relaxed);
  if (sink_ != nullptr) {
    sink_->on_event(destination_id, event);
  }
}

void MidiSequencer::dispatch_transformed(uint32_t destination_id, const MidiEvent& event,
                                         bool from_clip, uint32_t clip_id) noexcept {
  if (event.ump.is_note_on()) {
    if (!track_note_on(event.ump.group, event.ump.channel(), event.ump.note_number(),
                       destination_id, from_clip, clip_id)) {
      return;
    }
  } else if (event.ump.is_note_off()) {
    track_note_off(event.ump.group, event.ump.channel(), event.ump.note_number(), destination_id,
                   from_clip, clip_id);
  }
  dispatch(destination_id, event);
}

void MidiSequencer::enqueue_pending(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                                    uint32_t clip_id) noexcept {
  if (pending_fx_count_ >= kMaxPendingFxEvents) {
    midi_fx_pending_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pending_fx_[pending_fx_count_++] = PendingFxEvent{destination_id, event, clip_id, from_clip};
}

void MidiSequencer::dispatch_pending(int64_t block_start_frame, int64_t block_end_frame) noexcept {
  size_t i = 0;
  while (i < pending_fx_count_) {
    const PendingFxEvent pending = pending_fx_[i];
    if (pending.event.render_frame < block_start_frame) {
      dispatch_transformed(pending.destination_id, pending.event, pending.from_clip,
                           pending.clip_id);
    } else if (pending.event.render_frame < block_end_frame) {
      dispatch_transformed(pending.destination_id, pending.event, pending.from_clip,
                           pending.clip_id);
    } else {
      ++i;
      continue;
    }
    pending_fx_[i] = pending_fx_[pending_fx_count_ - 1];
    --pending_fx_count_;
  }
}

void MidiSequencer::clear_pending_for_destination(uint32_t destination_id) noexcept {
  size_t i = 0;
  while (i < pending_fx_count_) {
    if (pending_fx_[i].destination_id != destination_id) {
      ++i;
      continue;
    }
    pending_fx_[i] = pending_fx_[pending_fx_count_ - 1];
    --pending_fx_count_;
  }
}

void MidiSequencer::clear_pending_for_clip(uint32_t clip_id) noexcept {
  size_t i = 0;
  while (i < pending_fx_count_) {
    if (!pending_fx_[i].from_clip || pending_fx_[i].clip_id != clip_id) {
      ++i;
      continue;
    }
    pending_fx_[i] = pending_fx_[pending_fx_count_ - 1];
    --pending_fx_count_;
  }
}

void MidiSequencer::release_notes_for_clip(uint32_t clip_id, int64_t render_frame) noexcept {
  size_t i = 0;
  while (i < active_count_) {
    if (!active_[i].from_clip || active_[i].clip_id != clip_id) {
      ++i;
      continue;
    }
    const ActiveNote note = active_[i];
    MidiEvent off;
    off.render_frame = render_frame;
    off.ump = make_midi1_note_off(note.group, note.channel, note.note, 0);
    active_[i] = active_[active_count_ - 1];
    --active_count_;
    dispatch(note.destination_id, off);
  }
  clear_pending_for_clip(clip_id);
}

void MidiSequencer::process_event(uint32_t destination_id, const MidiEvent& event,
                                  int64_t block_end_frame, bool from_clip,
                                  uint32_t clip_id) noexcept {
  DestinationFx* fx = find_midi_fx(destination_id);
  if (fx == nullptr) {
    dispatch_transformed(destination_id, event, from_clip, clip_id);
    return;
  }
  fx->chain.process(&event, 1, &fx->buffer);
  for (size_t i = 0; i < fx->buffer.size; ++i) {
    const MidiEvent& transformed = fx->buffer.events[i];
    if (transformed.render_frame >= block_end_frame) {
      enqueue_pending(destination_id, transformed, from_clip, clip_id);
      continue;
    }
    dispatch_transformed(destination_id, transformed, from_clip, clip_id);
  }
}

void MidiSequencer::process_block(int64_t block_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t block_end_frame = block_start_frame + num_frames;
  dispatch_pending(block_start_frame, block_end_frame);
  const std::vector<MidiClipSchedule>* clips = clips_.current();
  if (clips == nullptr) return;

  for (const MidiClipSchedule& clip : *clips) {
    if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
      const int64_t loop_len = clip.loop_length_samples;
      const int64_t clip_end_frame =
          clip.length_samples > 0 ? clip.start_sample + clip.length_samples : block_end_frame;
      const int64_t scan_start = std::max(block_start_frame, clip.start_sample);
      const int64_t scan_end = std::min(block_end_frame, clip_end_frame);
      if (scan_start >= scan_end) continue;

      int64_t iter = (scan_start - clip.start_sample) / loop_len;
      for (int64_t iter_start = clip.start_sample + iter * loop_len; iter_start < scan_end;
           ++iter, iter_start += loop_len) {
        const int64_t iter_end = iter_start + loop_len;
        for (const MidiEvent& ev : clip.events) {
          const int64_t local = ev.render_frame - clip.start_sample;
          if (local < 0) continue;
          if (local >= loop_len) break;
          MidiEvent looped = ev;
          looped.render_frame = iter_start + local;
          if (looped.render_frame < block_start_frame) continue;
          if (looped.render_frame >= block_end_frame) break;
          if (looped.render_frame >= clip_end_frame) break;
          process_event(clip.destination_id, looped, block_end_frame, /*from_clip=*/true, clip.id);
        }
        if (iter_end > block_start_frame && iter_end <= block_end_frame &&
            iter_end <= clip_end_frame) {
          release_notes_for_clip(clip.id, iter_end);
        }
      }
      if (clip.length_samples > 0 && clip_end_frame > block_start_frame &&
          clip_end_frame <= block_end_frame) {
        release_notes_for_clip(clip.id, clip_end_frame);
      }
      continue;
    }

    const bool one_shot = clip.loop_mode == MidiLoopMode::kOneShot;
    const bool finite_one_shot = one_shot && clip.length_samples > 0;
    const int64_t clip_end_frame = clip.start_sample + clip.length_samples;
    if (finite_one_shot && clip_end_frame <= block_start_frame) continue;

    // Events are sorted ascending by render_frame; scan the in-block window.
    for (const MidiEvent& ev : clip.events) {
      if (ev.render_frame < block_start_frame) continue;
      if (ev.render_frame >= block_end_frame) break;
      if (finite_one_shot && ev.render_frame >= clip_end_frame) break;
      process_event(clip.destination_id, ev, block_end_frame, /*from_clip=*/true, clip.id);
    }

    if (finite_one_shot && clip_end_frame > block_start_frame &&
        clip_end_frame <= block_end_frame) {
      release_notes_for_clip(clip.id, clip_end_frame);
    }
  }
}

void MidiSequencer::emit_controller_reset(uint32_t destination_id, uint8_t group, uint8_t channel,
                                          int64_t render_frame) noexcept {
  // Standard MIDI reset on a playback discontinuity. Channel-mode controllers
  // 64 (damper), 121 (reset all controllers), 123 (all notes off) plus a
  // pitch-bend recenter. Dispatched raw (not through MIDI FX) at render_frame.
  static constexpr uint8_t kDamperPedal = 64;
  static constexpr uint8_t kResetAllControllers = 121;
  static constexpr uint8_t kAllNotesOff = 123;
  static constexpr uint16_t kPitchBendCenter = 8192;
  MidiEvent ev;
  ev.render_frame = render_frame;
  ev.ump = make_midi1_control_change(group, channel, kDamperPedal, 0);
  dispatch(destination_id, ev);
  ev.ump = make_midi1_control_change(group, channel, kResetAllControllers, 0);
  dispatch(destination_id, ev);
  ev.ump = make_midi1_control_change(group, channel, kAllNotesOff, 0);
  dispatch(destination_id, ev);
  ev.ump = make_midi1_pitch_bend(group, channel, kPitchBendCenter);
  dispatch(destination_id, ev);
}

void MidiSequencer::emit_active_controller_resets(bool single_destination, uint32_t destination_id,
                                                  int64_t render_frame) noexcept {
  for (size_t i = 0; i < active_count_; ++i) {
    const ActiveNote& note = active_[i];
    if (single_destination && note.destination_id != destination_id) continue;
    // Reset each channel once: skip if an earlier slot already covered this
    // (destination, group, channel) triple.
    bool seen = false;
    for (size_t j = 0; j < i; ++j) {
      if (active_[j].destination_id == note.destination_id && active_[j].group == note.group &&
          active_[j].channel == note.channel) {
        seen = true;
        break;
      }
    }
    if (!seen) emit_controller_reset(note.destination_id, note.group, note.channel, render_frame);
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
  // Controller reset AFTER the note-offs (so a note under a held damper is first
  // told to stop, then the damper is lifted). Table is still intact here. (M-4)
  emit_active_controller_resets(/*single_destination=*/false, 0, render_frame);
  active_count_ = 0;
  pending_fx_count_ = 0;
}

void MidiSequencer::all_notes_off_for_destination(uint32_t destination_id,
                                                  int64_t render_frame) noexcept {
  // Release only the notes sounding on `destination_id` (hang-note safety when a
  // single instrument is swapped or cleared on its destination, leaving notes on
  // other destinations untouched). Swap-remove keeps the table compact; iterate
  // by index and re-check the same slot after a swap. No allocation.
  // Reset this destination's channels first while the table is still intact,
  // then release its notes (lift damper / recenter bend before the offs) (M-4).
  emit_active_controller_resets(/*single_destination=*/true, destination_id, render_frame);
  size_t i = 0;
  while (i < active_count_) {
    if (active_[i].destination_id != destination_id) {
      ++i;
      continue;
    }
    const ActiveNote note = active_[i];
    MidiEvent off;
    off.render_frame = render_frame;
    off.ump = make_midi1_note_off(note.group, note.channel, note.note, 0);
    // Drop the entry first so dispatch (and any re-entrant query) sees a
    // consistent table, then emit the note-off.
    active_[i] = active_[active_count_ - 1];
    --active_count_;
    dispatch(destination_id, off);
  }
  clear_pending_for_destination(destination_id);
}

void MidiSequencer::inject_event(uint32_t destination_id, int64_t render_frame,
                                 const Ump& ump) noexcept {
  // Mirror process_block's active-note bookkeeping so a live note-on/off keeps
  // the hang-note table consistent, then dispatch at the requested render frame.
  MidiEvent event;
  event.render_frame = render_frame;
  event.ump = ump;
  process_event(destination_id, event, render_frame + 1, /*from_clip=*/false, 0);
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

  auto push_offset = [out](int offset) noexcept {
    for (size_t i = 0; i < out->size; ++i) {
      if (out->offsets[i] == offset) return;
    }
    if (out->size >= BoundaryOffsets::kCapacity) {
      out->overflowed = true;
      return;
    }
    size_t pos = 0;
    while (pos < out->size && out->offsets[pos] < offset) {
      ++pos;
    }
    for (size_t i = out->size; i > pos; --i) {
      out->offsets[i] = out->offsets[i - 1];
    }
    out->offsets[pos] = offset;
    ++out->size;
  };

  for (const MidiClipSchedule& clip : *clips) {
    if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
      const int64_t loop_len = clip.loop_length_samples;
      const int64_t clip_end_frame =
          clip.length_samples > 0 ? clip.start_sample + clip.length_samples : block_end_frame;
      const int64_t scan_start = std::max(block_start_frame, clip.start_sample);
      const int64_t scan_end = std::min(block_end_frame, clip_end_frame);
      if (scan_start >= scan_end) continue;

      int64_t iter = (scan_start - clip.start_sample) / loop_len;
      for (int64_t iter_start = clip.start_sample + iter * loop_len; iter_start < scan_end;
           ++iter, iter_start += loop_len) {
        const int64_t iter_end = iter_start + loop_len;
        for (const MidiEvent& ev : clip.events) {
          const int64_t local = ev.render_frame - clip.start_sample;
          if (local < 0) continue;
          if (local >= loop_len) break;
          const int64_t render_frame = iter_start + local;
          if (render_frame < block_start_frame) continue;
          if (render_frame >= block_end_frame) break;
          if (render_frame >= clip_end_frame) break;
          push_offset(static_cast<int>(render_frame - block_start_frame));
        }
        if (iter_end > block_start_frame && iter_end < block_end_frame &&
            iter_end <= clip_end_frame) {
          push_offset(static_cast<int>(iter_end - block_start_frame));
        }
      }
      if (clip.length_samples > 0 && clip_end_frame > block_start_frame &&
          clip_end_frame < block_end_frame) {
        push_offset(static_cast<int>(clip_end_frame - block_start_frame));
      }
      continue;
    }

    const bool finite_one_shot =
        clip.loop_mode == MidiLoopMode::kOneShot && clip.length_samples > 0;
    const int64_t clip_end_frame = clip.start_sample + clip.length_samples;
    for (const MidiEvent& ev : clip.events) {
      if (ev.render_frame < block_start_frame) continue;
      if (ev.render_frame >= block_end_frame) break;
      if (finite_one_shot && ev.render_frame >= clip_end_frame) break;
      push_offset(static_cast<int>(ev.render_frame - block_start_frame));
    }
    if (finite_one_shot && clip_end_frame > block_start_frame && clip_end_frame < block_end_frame) {
      push_offset(static_cast<int>(clip_end_frame - block_start_frame));
    }
  }
}

}  // namespace sonare::midi
