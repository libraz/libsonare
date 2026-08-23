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
  last_clips_ = nullptr;
  last_midi_fx_snapshot_ = nullptr;
  dispatched_event_count_.store(0, std::memory_order_relaxed);
}

void MidiSequencer::set_midi_clips(std::vector<MidiClipSchedule> clips) {
  // Bring every incoming event into the one group basis before it is published.
  // Ump::group is a cache of word[0] bits 24..27, and the two halves are read by
  // different consumers: routing, active-note tracking and MIDI FX read the
  // cached field, while the bytes that reach a device or an SMF2 file come from
  // word0. A caller that supplies them separately -- which the binding surfaces
  // do, with the group defaulting to 0 while word0 is authored -- can hand over
  // a pair that disagrees, and every consumer downstream would then see a
  // different group for the same message. word0 wins because it is the form that
  // leaves the process. Doing it here rather than in a binding covers every
  // entry point in one place: the C ABI, the WASM wrappers that call the core
  // directly, and the arrangement compiler all publish through this function.
  //
  // This loop is the ENFORCING site for that invariant. The C ABI and the WASM
  // wrapper each derive the group again at their own boundary, but only so their
  // local conversion is self-consistent; removing either leaves the tests green,
  // while removing this one turns them red. Anything relying on the group being
  // normalized is relying on this loop.
  for (MidiClipSchedule& clip : clips) {
    for (MidiEvent& event : clip.events) {
      event.ump.group = ump_group_from_word0(event.ump.words[0]);
    }
  }
  const size_t count = clips.size();
  auto snapshot = std::make_shared<const std::vector<MidiClipSchedule>>(std::move(clips));
  if (clips_.publish(std::move(snapshot))) {
    clip_count_.store(count, std::memory_order_relaxed);
  }
}

bool MidiSequencer::track_note_on(uint8_t group, uint8_t channel, uint8_t note,
                                  uint32_t destination_id, uint32_t source_track_id, bool from_clip,
                                  uint32_t clip_id) noexcept {
  if (active_count_ >= kMaxActiveNotes) {
    active_note_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  active_[active_count_] =
      ActiveNote{group, channel, note, destination_id, source_track_id, clip_id, from_clip};
  ++active_count_;
  return true;
}

void MidiSequencer::track_note_off(uint8_t group, uint8_t channel, uint8_t note,
                                   uint32_t destination_id, uint32_t source_track_id,
                                   bool from_clip, uint32_t clip_id) noexcept {
  size_t fallback = kMaxActiveNotes;
  for (size_t i = 0; i < active_count_; ++i) {
    if (active_[i].group == group && active_[i].channel == channel && active_[i].note == note &&
        active_[i].destination_id == destination_id) {
      if (active_[i].source_track_id == source_track_id && active_[i].from_clip == from_clip &&
          active_[i].clip_id == clip_id) {
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

bool MidiSequencer::set_midi_fx(uint32_t destination_id, const MidiFxChain& chain,
                                int64_t render_frame) noexcept {
  (void)render_frame;
  try {
    auto next = std::make_shared<MidiFxSnapshot>();
    if (const std::shared_ptr<const MidiFxSnapshot>& current =
            midi_fx_snapshots_.control_current()) {
      *next = *current;
    }
    DestinationFxConfig* slot = nullptr;
    for (DestinationFxConfig& candidate : next->destinations) {
      if (candidate.active && candidate.destination_id == destination_id) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) {
      for (DestinationFxConfig& candidate : next->destinations) {
        if (!candidate.active) {
          slot = &candidate;
          break;
        }
      }
    }
    if (slot == nullptr) return false;
    slot->active = true;
    slot->destination_id = destination_id;
    slot->generation = next_midi_fx_generation_++;
    if (next_midi_fx_generation_ == 0) next_midi_fx_generation_ = 1;
    slot->transpose = chain.transpose();
    slot->quantize = chain.quantize();
    slot->velocity = chain.velocity_curve();
    slot->chord = chain.chord();
    slot->arpeggiator = chain.arpeggiator();
    slot->humanize = chain.humanize();
    return midi_fx_snapshots_.publish(std::shared_ptr<const MidiFxSnapshot>(std::move(next)));
  } catch (...) {
    return false;
  }
}

void MidiSequencer::clear_midi_fx(uint32_t destination_id) noexcept {
  try {
    const std::shared_ptr<const MidiFxSnapshot>& current = midi_fx_snapshots_.control_current();
    if (!current) return;
    auto next = std::make_shared<MidiFxSnapshot>(*current);
    for (DestinationFxConfig& slot : next->destinations) {
      if (!slot.active || slot.destination_id != destination_id) continue;
      slot.active = false;
      slot.destination_id = 0;
      slot.generation = next_midi_fx_generation_++;
      if (next_midi_fx_generation_ == 0) next_midi_fx_generation_ = 1;
      midi_fx_snapshots_.publish(std::shared_ptr<const MidiFxSnapshot>(std::move(next)));
      return;
    }
  } catch (...) {
    // Keep the current published configuration on allocation failure.
  }
}

void MidiSequencer::acquire_midi_fx(int64_t render_frame) noexcept {
  midi_fx_snapshots_.acquire();
  const MidiFxSnapshot* snapshot = midi_fx_snapshots_.current();
  if (snapshot == last_midi_fx_snapshot_) return;

  // Flush every live destination whose exact configuration generation is not
  // present in the new snapshot. This runs on the audio thread, so sink calls
  // are correctly ordered before the replacement becomes visible.
  for (const DestinationFx& live : midi_fx_) {
    if (!live.active) continue;
    bool unchanged = false;
    if (snapshot != nullptr) {
      for (const DestinationFxConfig& config : snapshot->destinations) {
        if (config.active && config.destination_id == live.destination_id &&
            config.generation == live.generation) {
          unchanged = true;
          break;
        }
      }
    }
    if (!unchanged) all_notes_off_for_destination(live.destination_id, render_frame);
  }

  for (DestinationFx& live : midi_fx_) {
    live.active = false;
    live.destination_id = 0;
    live.generation = 0;
    live.buffer.clear();
    live.next_input_ordinal = 0;
  }
  if (snapshot != nullptr) {
    for (size_t i = 0; i < snapshot->destinations.size(); ++i) {
      const DestinationFxConfig& config = snapshot->destinations[i];
      if (!config.active) continue;
      DestinationFx& live = midi_fx_[i];
      live.active = true;
      live.destination_id = config.destination_id;
      live.generation = config.generation;
      live.chain.set_transpose(config.transpose);
      live.chain.set_quantize(config.quantize);
      live.chain.set_velocity_curve(config.velocity);
      live.chain.set_chord(config.chord);
      live.chain.set_arpeggiator(config.arpeggiator);
      live.chain.set_humanize(config.humanize);
      live.chain.prepare();
      live.next_input_ordinal = 0;
    }
  }
  last_midi_fx_snapshot_ = snapshot;
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
                       destination_id, event.source_track_id, from_clip, clip_id)) {
      return;
    }
  } else if (event.ump.is_note_off()) {
    track_note_off(event.ump.group, event.ump.channel(), event.ump.note_number(), destination_id,
                   event.source_track_id, from_clip, clip_id);
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

void MidiSequencer::dispatch_pending_through(int64_t block_start_frame, int64_t block_end_frame,
                                             int64_t through_frame) noexcept {
  for (;;) {
    size_t selected = pending_fx_count_;
    int64_t selected_frame = 0;
    for (size_t i = 0; i < pending_fx_count_; ++i) {
      const int64_t frame = std::max(pending_fx_[i].event.render_frame, block_start_frame);
      if (frame >= block_end_frame || frame > through_frame) continue;
      if (selected == pending_fx_count_ || frame < selected_frame) {
        selected = i;
        selected_frame = frame;
      }
    }
    if (selected == pending_fx_count_) return;

    PendingFxEvent pending = pending_fx_[selected];
    // An event carried over from an earlier block still holds that block's
    // render_frame; clamp it to the current block start so sample-accurate
    // consumers never see a timestamp in the past. (BuiltinSynth ignores the
    // frame, but external/sample-accurate instruments would mis-place it.)
    if (pending.event.render_frame < block_start_frame) {
      pending.event.render_frame = block_start_frame;
    }
    dispatch_transformed(pending.destination_id, pending.event, pending.from_clip, pending.clip_id);
    pending_fx_[selected] = pending_fx_[pending_fx_count_ - 1];
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

void MidiSequencer::release_notes_for_clip(uint32_t clip_id, int64_t render_frame,
                                           bool clear_pending) noexcept {
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
    off.source_track_id = note.source_track_id;
    active_[i] = active_[active_count_ - 1];
    --active_count_;
    dispatch(note.destination_id, off);
  }
  if (clear_pending) {
    clear_pending_for_clip(clip_id);
  }
}

void MidiSequencer::release_notes_for_absent_clips(const std::vector<MidiClipSchedule>* clips,
                                                   int64_t render_frame) noexcept {
  const auto present = [clips](uint32_t clip_id) noexcept -> bool {
    if (clips == nullptr) return false;
    for (const MidiClipSchedule& c : *clips) {
      if (c.id == clip_id) return true;
    }
    return false;
  };
  size_t i = 0;
  while (i < active_count_) {
    if (!active_[i].from_clip || present(active_[i].clip_id)) {
      ++i;
      continue;
    }
    const ActiveNote note = active_[i];
    MidiEvent off;
    off.render_frame = render_frame;
    off.ump = make_midi1_note_off(note.group, note.channel, note.note, 0);
    off.source_track_id = note.source_track_id;
    active_[i] = active_[active_count_ - 1];
    --active_count_;
    dispatch(note.destination_id, off);
  }
  size_t p = 0;
  while (p < pending_fx_count_) {
    if (!pending_fx_[p].from_clip || present(pending_fx_[p].clip_id)) {
      ++p;
      continue;
    }
    pending_fx_[p] = pending_fx_[pending_fx_count_ - 1];
    --pending_fx_count_;
  }
}

void MidiSequencer::process_event(uint32_t destination_id, const MidiEvent& event,
                                  int64_t block_end_frame, bool from_clip,
                                  uint32_t clip_id) noexcept {
  DestinationFx* fx = find_midi_fx(destination_id);
  if (fx == nullptr) {
    dispatch_transformed(destination_id, event, from_clip, clip_id);
    return;
  }
  fx->chain.process_chunk(&event, 1, fx->next_input_ordinal++, &fx->buffer);
  for (size_t i = 0; i < fx->buffer.size; ++i) {
    const MidiEvent& transformed = fx->buffer.events[i];
    // Generated future events must rejoin the sequencer's chronological merge
    // even when they still fall inside this block. Dispatching them immediately
    // would let an arpeggiator step leapfrog an earlier event from another clip.
    if (transformed.render_frame > event.render_frame ||
        transformed.render_frame >= block_end_frame) {
      enqueue_pending(destination_id, transformed, from_clip, clip_id);
      continue;
    }
    dispatch_transformed(destination_id, transformed, from_clip, clip_id);
  }
}

void MidiSequencer::process_block(int64_t block_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t block_end_frame = block_start_frame + num_frames;
  const std::vector<MidiClipSchedule>* clips = clips_.current();
  if (clips != last_clips_) {
    // The published clip set changed (a live mute, clip delete, or edit
    // recompiled and republished). Release notes still sounding from clips that
    // are no longer present -- and drop their pending FX events -- so a muted or
    // deleted MIDI clip does not hang a note. Runs before dispatch_pending so a
    // removed clip's carried-over events are not fired. Idempotent when nothing
    // was removed (a republished set with the same clip ids releases nothing).
    release_notes_for_absent_clips(clips, block_start_frame);
    last_clips_ = clips;
  }
  // Visit every clip event and synthetic clip/loop end in the block. The
  // visitor is allocation-free; process_block uses it first to select the next
  // render frame, then again to dispatch every item at that frame. This
  // selection merge is O(events^2), but event counts are bounded by the
  // compiled clips and fixed MIDI-FX pending table, and it keeps all dispatches
  // monotonic without an audio-thread sort buffer.
  auto visit_scheduled = [&](auto&& visitor) noexcept {
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
          for (const MidiEvent& event : clip.events) {
            const int64_t local = event.render_frame - clip.start_sample;
            if (local < 0) continue;
            if (local >= loop_len) break;
            const int64_t frame = iter_start + local;
            if (frame < block_start_frame) continue;
            if (frame >= block_end_frame || frame >= clip_end_frame) break;
            visitor(clip, &event, frame, false);
          }
          if (iter_end > block_start_frame && iter_end < block_end_frame &&
              iter_end <= clip_end_frame) {
            visitor(clip, nullptr, iter_end, false);
          }
        }
        if (clip.length_samples > 0 && clip_end_frame > block_start_frame &&
            clip_end_frame < block_end_frame) {
          visitor(clip, nullptr, clip_end_frame, true);
        }
        continue;
      }

      const bool finite_one_shot =
          clip.loop_mode == MidiLoopMode::kOneShot && clip.length_samples > 0;
      const int64_t clip_end_frame = clip.start_sample + clip.length_samples;
      if (finite_one_shot && clip_end_frame <= block_start_frame) continue;
      for (const MidiEvent& event : clip.events) {
        if (event.render_frame < block_start_frame) continue;
        if (event.render_frame >= block_end_frame) break;
        if (finite_one_shot && event.render_frame >= clip_end_frame) break;
        visitor(clip, &event, event.render_frame, false);
      }
      if (finite_one_shot && clip_end_frame > block_start_frame &&
          clip_end_frame < block_end_frame) {
        visitor(clip, nullptr, clip_end_frame, true);
      }
    }
  };

  int64_t cursor = block_start_frame;
  while (cursor < block_end_frame) {
    int64_t next_frame = block_end_frame;
    for (size_t i = 0; i < pending_fx_count_; ++i) {
      const int64_t frame = std::max(pending_fx_[i].event.render_frame, block_start_frame);
      if (frame >= cursor && frame < next_frame) next_frame = frame;
    }
    visit_scheduled([&](const MidiClipSchedule&, const MidiEvent*, int64_t frame, bool) noexcept {
      if (frame >= cursor && frame < next_frame) next_frame = frame;
    });
    if (next_frame >= block_end_frame) break;

    dispatch_pending_through(block_start_frame, block_end_frame, next_frame);
    visit_scheduled([&](const MidiClipSchedule& clip, const MidiEvent* event, int64_t frame,
                        bool clear_pending) noexcept {
      if (frame != next_frame) return;
      if (event != nullptr) {
        MidiEvent scheduled = *event;
        scheduled.render_frame = frame;
        scheduled.source_track_id = clip.track_id;
        process_event(clip.destination_id, scheduled, block_end_frame, /*from_clip=*/true, clip.id);
      } else {
        release_notes_for_clip(clip.id, frame, clear_pending);
      }
    });
    cursor = next_frame + 1;
  }

  // Clip-end note releases historically occur exactly at the exclusive block
  // boundary. Keep that contract while leaving pending/generated events at the
  // same frame for the next block.
  if (clips != nullptr) {
    for (const MidiClipSchedule& clip : *clips) {
      if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
        const int64_t loop_len = clip.loop_length_samples;
        const int64_t clip_end_frame =
            clip.length_samples > 0 ? clip.start_sample + clip.length_samples : block_end_frame;
        if (block_end_frame > clip.start_sample &&
            (block_end_frame - clip.start_sample) % loop_len == 0 &&
            block_end_frame <= clip_end_frame) {
          release_notes_for_clip(clip.id, block_end_frame, /*clear_pending=*/false);
        }
        if (clip.length_samples > 0 && clip_end_frame == block_end_frame) {
          release_notes_for_clip(clip.id, block_end_frame);
        }
      } else if (clip.loop_mode == MidiLoopMode::kOneShot && clip.length_samples > 0 &&
                 clip.start_sample + clip.length_samples == block_end_frame) {
        release_notes_for_clip(clip.id, block_end_frame);
      }
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
    off.source_track_id = note.source_track_id;
    dispatch(note.destination_id, off);
  }
  // Controller reset AFTER the note-offs (so a note under a held damper is first
  // told to stop, then the damper is lifted). Table is still intact here.
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
  // then release its notes (lift damper / recenter bend before the offs).
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
    off.source_track_id = note.source_track_id;
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

void MidiSequencer::inject_event(uint32_t destination_id, int64_t render_frame, const Ump& ump,
                                 const uint8_t* sysex_payload, size_t sysex_payload_size) noexcept {
  MidiEvent event;
  event.render_frame = render_frame;
  event.ump = ump;
  event.sysex_payload = sysex_payload;
  event.sysex_payload_size = sysex_payload_size;
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
