#include "midi/sequencer.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include "midi/ump.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::midi {

namespace {

bool is_midi_channel_voice(const Ump& ump) noexcept {
  return ump.message_type() == UmpMessageType::kMidi1ChannelVoice ||
         ump.message_type() == UmpMessageType::kMidi2ChannelVoice;
}

bool is_channel_mode_reset(const Ump& ump) noexcept {
  if (!is_midi_channel_voice(ump) ||
      ump.status_nibble() != static_cast<uint8_t>(UmpStatus::kControlChange)) {
    return false;
  }
  const uint8_t controller = ump.note_number();
  return controller == 120 || (controller >= 123 && controller <= 127);
}

int64_t saturating_clip_end(const MidiClipSchedule& clip) noexcept {
  if (clip.length_samples <= 0) return std::numeric_limits<int64_t>::max();
  const int64_t max = std::numeric_limits<int64_t>::max();
  if (clip.start_sample > max - clip.length_samples) return max;
  return clip.start_sample + clip.length_samples;
}

bool clip_contains_frame(const MidiClipSchedule& clip, int64_t frame) noexcept {
  return frame >= clip.start_sample && frame < saturating_clip_end(clip);
}

/// Offset of @p frame into its loop iteration; exact for any frame >= clip.start_sample.
int64_t loop_phase(const MidiClipSchedule& clip, int64_t frame) noexcept {
  const uint64_t elapsed = static_cast<uint64_t>(frame) - static_cast<uint64_t>(clip.start_sample);
  return static_cast<int64_t>(elapsed % static_cast<uint64_t>(clip.loop_length_samples));
}

/// First event of @p clip at or after @p frame; clip events are sorted by render_frame.
std::vector<MidiEvent>::const_iterator first_event_at(const MidiClipSchedule& clip,
                                                      int64_t frame) noexcept {
  return std::lower_bound(
      clip.events.begin(), clip.events.end(), frame,
      [](const MidiEvent& event, int64_t value) noexcept { return event.render_frame < value; });
}

/// First event of a looping @p clip that lands at or after @p frame in the iteration
/// starting at @p iter_start.
std::vector<MidiEvent>::const_iterator first_loop_event_at(const MidiClipSchedule& clip,
                                                           int64_t iter_start,
                                                           int64_t frame) noexcept {
  const int64_t local =
      std::clamp<int64_t>(numeric::saturating_sub(frame, iter_start), 0, clip.loop_length_samples);
  return first_event_at(clip, numeric::saturating_add(clip.start_sample, local));
}

}  // namespace

void MidiSequencer::prepare(double sample_rate) {
  // Allocated once before any state changes, so a failure leaves the sequencer intact.
  if (runtime_storage_ == nullptr) {
    auto next_storage = std::make_unique<RuntimeStorage>();
    runtime_storage_ = std::move(next_storage);
  }
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  reset();
}

void MidiSequencer::reset() noexcept {
  active_count_ = 0;
  pending_fx_count_ = 0;
  if (runtime_storage_ != nullptr) {
    for (DestinationFx& fx : runtime_storage_->midi_fx) {
      fx.chain.reset();
      fx.buffer.clear();
      fx.next_input_ordinal = 0;
    }
  }
  for (RetainedChannelState& state : retained_channels_) state = RetainedChannelState{};
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
  MidiSysExPayloadError payload_error = MidiSysExPayloadError::kNone;
  if (!own_sysex_payloads(clips, &payload_error)) {
    throw SonareException(payload_error == MidiSysExPayloadError::kOutOfMemory
                              ? ErrorCode::OutOfMemory
                              : ErrorCode::InvalidParameter,
                          describe(payload_error));
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
      // Fallback match only within the same source track; never steal another track's note.
      if (active_[i].source_track_id == source_track_id && fallback == kMaxActiveNotes) {
        fallback = i;
      }
    }
  }
  if (fallback != kMaxActiveNotes) {
    active_[fallback] = active_[active_count_ - 1];
    --active_count_;
  }
}

bool MidiSequencer::retain_channel_state(uint32_t destination_id, uint8_t group, uint8_t channel,
                                         bool* inserted) noexcept {
  if (inserted != nullptr) *inserted = false;
  for (const RetainedChannelState& state : retained_channels_) {
    if (state.active && state.destination_id == destination_id && state.group == group &&
        state.channel == channel) {
      return true;
    }
  }
  for (RetainedChannelState& state : retained_channels_) {
    if (state.active) continue;
    state.active = true;
    state.destination_id = destination_id;
    state.group = group;
    state.channel = channel;
    if (inserted != nullptr) *inserted = true;
    return true;
  }
  retained_channel_overflow_count_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void MidiSequencer::release_retained_channel_state(uint32_t destination_id, uint8_t group,
                                                   uint8_t channel, bool inserted) noexcept {
  if (!inserted) return;
  for (RetainedChannelState& state : retained_channels_) {
    if (!state.active || state.destination_id != destination_id || state.group != group ||
        state.channel != channel) {
      continue;
    }
    state = RetainedChannelState{};
    return;
  }
}

void MidiSequencer::clear_retained_channel_states(bool single_destination,
                                                  uint32_t destination_id) noexcept {
  for (RetainedChannelState& state : retained_channels_) {
    if (!state.active || (single_destination && state.destination_id != destination_id)) continue;
    state = RetainedChannelState{};
  }
}

MidiSequencer::DestinationFx* MidiSequencer::find_midi_fx(uint32_t destination_id) noexcept {
  if (runtime_storage_ == nullptr) return nullptr;
  for (DestinationFx& fx : runtime_storage_->midi_fx) {
    if (fx.active && fx.destination_id == destination_id) return &fx;
  }
  return nullptr;
}

const MidiSequencer::DestinationFx* MidiSequencer::find_midi_fx(
    uint32_t destination_id) const noexcept {
  if (runtime_storage_ == nullptr) return nullptr;
  for (const DestinationFx& fx : runtime_storage_->midi_fx) {
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
  if (runtime_storage_ == nullptr) return;
  midi_fx_snapshots_.acquire();
  const MidiFxSnapshot* snapshot = midi_fx_snapshots_.current();
  if (snapshot == last_midi_fx_snapshot_) return;

  // Flush every live destination whose exact configuration generation is not
  // present in the new snapshot. Unchanged destinations stay in place so their
  // pending note timings and input ordinals survive an unrelated destination
  // edit. This runs on the audio thread, so sink calls are correctly ordered
  // before the replacement becomes visible.
  for (DestinationFx& live : runtime_storage_->midi_fx) {
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
    if (unchanged) continue;
    all_notes_off_for_destination(live.destination_id, render_frame);
    live.chain.reset();
    live.active = false;
    live.destination_id = 0;
    live.generation = 0;
    live.buffer.clear();
    live.next_input_ordinal = 0;
  }

  if (snapshot != nullptr) {
    for (const DestinationFxConfig& config : snapshot->destinations) {
      if (!config.active) continue;

      DestinationFx* live = nullptr;
      for (DestinationFx& candidate : runtime_storage_->midi_fx) {
        if (candidate.active && candidate.destination_id == config.destination_id &&
            candidate.generation == config.generation) {
          live = &candidate;
          break;
        }
      }
      if (live != nullptr) continue;

      for (DestinationFx& candidate : runtime_storage_->midi_fx) {
        if (!candidate.active) {
          live = &candidate;
          break;
        }
      }
      if (live == nullptr) continue;
      live->active = true;
      live->destination_id = config.destination_id;
      live->generation = config.generation;
      live->chain.set_transpose(config.transpose);
      live->chain.set_quantize(config.quantize);
      live->chain.set_velocity_curve(config.velocity);
      live->chain.set_chord(config.chord);
      live->chain.set_arpeggiator(config.arpeggiator);
      live->chain.set_humanize(config.humanize);
      live->chain.prepare();
      live->buffer.clear();
      live->next_input_ordinal = 0;
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

void MidiSequencer::clear_active_notes_for_channel(uint32_t destination_id, uint8_t group,
                                                   uint8_t channel) noexcept {
  size_t i = 0;
  while (i < active_count_) {
    if (active_[i].destination_id != destination_id || active_[i].group != group ||
        active_[i].channel != channel) {
      ++i;
      continue;
    }
    active_[i] = active_[active_count_ - 1];
    --active_count_;
  }
}

void MidiSequencer::clear_note_tracking_for_event(uint32_t destination_id, const MidiEvent& event,
                                                  bool from_clip, uint32_t clip_id) noexcept {
  if (!is_midi_channel_voice(event.ump) || (!event.ump.is_note_on() && !event.ump.is_note_off())) {
    return;
  }
  DestinationFx* fx = find_midi_fx(destination_id);
  if (fx != nullptr) {
    fx->chain.clear_note_tracking(event.ump.group, event.ump.channel(), event.ump.note_number(),
                                  event.source_track_id, from_clip, clip_id);
  }
}

void MidiSequencer::clear_pending_note_tracking_for_event(const PendingFxEvent& pending) noexcept {
  DestinationFx* fx = find_midi_fx(pending.destination_id);
  if (fx != nullptr) {
    fx->chain.clear_pending_note_tracking(pending.event, pending.from_clip, pending.clip_id);
  }
}

void MidiSequencer::clear_pending_note_events_for_channel(uint32_t destination_id, uint8_t group,
                                                          uint8_t channel) noexcept {
  if (runtime_storage_ == nullptr) return;
  size_t i = 0;
  while (i < pending_fx_count_) {
    const PendingFxEvent& pending = runtime_storage_->pending_fx[i];
    if (pending.destination_id != destination_id || pending.event.ump.group != group ||
        pending.event.ump.channel() != channel || !is_midi_channel_voice(pending.event.ump) ||
        (!pending.event.ump.is_note_on() && !pending.event.ump.is_note_off())) {
      ++i;
      continue;
    }
    clear_pending_note_tracking_for_event(pending);
    erase_pending(i);
  }
}

void MidiSequencer::retire_channel_mode_reset(uint32_t destination_id, uint8_t group,
                                              uint8_t channel) noexcept {
  clear_active_notes_for_channel(destination_id, group, channel);
  clear_pending_note_events_for_channel(destination_id, group, channel);
  DestinationFx* fx = find_midi_fx(destination_id);
  if (fx != nullptr) fx->chain.clear_note_tracking(group, channel);
  // Keep retained_channels_: a later seek, stop or route change still owes the full reset.
}

void MidiSequencer::dispatch_transformed(uint32_t destination_id, const MidiEvent& event,
                                         bool from_clip, uint32_t clip_id) noexcept {
  const bool channel_voice = is_midi_channel_voice(event.ump);
  const bool semantic_note_off = channel_voice && event.ump.is_note_off();
  bool inserted_channel_state = false;
  if (channel_voice && !semantic_note_off &&
      !retain_channel_state(destination_id, event.ump.group, event.ump.channel(),
                            &inserted_channel_state)) {
    // Ledger full: forwarding would leave a channel no later reset can reach.
    return;
  }
  if (event.ump.is_note_on()) {
    if (!track_note_on(event.ump.group, event.ump.channel(), event.ump.note_number(),
                       destination_id, event.source_track_id, from_clip, clip_id)) {
      release_retained_channel_state(destination_id, event.ump.group, event.ump.channel(),
                                     inserted_channel_state);
      return;
    }
  } else if (event.ump.is_note_off()) {
    track_note_off(event.ump.group, event.ump.channel(), event.ump.note_number(), destination_id,
                   event.source_track_id, from_clip, clip_id);
  }
  if (is_channel_mode_reset(event.ump)) {
    retire_channel_mode_reset(destination_id, event.ump.group, event.ump.channel());
  }
  dispatch(destination_id, event);
}

void MidiSequencer::enqueue_pending(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                                    uint32_t clip_id) noexcept {
  if (runtime_storage_ == nullptr || pending_fx_count_ >= kMaxPendingFxEvents) {
    midi_fx_pending_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  runtime_storage_->pending_fx[pending_fx_count_++] =
      PendingFxEvent{destination_id, event, clip_id, from_clip};
}

void MidiSequencer::erase_pending(size_t index) noexcept {
  if (runtime_storage_ == nullptr || index >= pending_fx_count_) return;
  for (size_t i = index + 1; i < pending_fx_count_; ++i) {
    runtime_storage_->pending_fx[i - 1] = runtime_storage_->pending_fx[i];
  }
  --pending_fx_count_;
}

void MidiSequencer::clear_pending_for_destination(uint32_t destination_id) noexcept {
  if (runtime_storage_ == nullptr) return;
  size_t i = 0;
  while (i < pending_fx_count_) {
    if (runtime_storage_->pending_fx[i].destination_id != destination_id) {
      ++i;
      continue;
    }
    clear_pending_note_tracking_for_event(runtime_storage_->pending_fx[i]);
    erase_pending(i);
  }
}

void MidiSequencer::clear_pending_for_clip(uint32_t clip_id) noexcept {
  if (runtime_storage_ == nullptr) return;
  size_t i = 0;
  while (i < pending_fx_count_) {
    if (!runtime_storage_->pending_fx[i].from_clip ||
        runtime_storage_->pending_fx[i].clip_id != clip_id) {
      ++i;
      continue;
    }
    clear_pending_note_tracking_for_event(runtime_storage_->pending_fx[i]);
    erase_pending(i);
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
    clear_note_tracking_for_event(note.destination_id, off, note.from_clip, note.clip_id);
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
  const auto present = [clips](uint32_t clip_id, uint32_t destination_id, uint32_t source_track_id,
                               int64_t frame) noexcept -> bool {
    if (clips == nullptr) return false;
    for (const MidiClipSchedule& c : *clips) {
      if (c.id == clip_id && c.destination_id == destination_id && c.track_id == source_track_id &&
          clip_contains_frame(c, frame)) {
        return true;
      }
    }
    return false;
  };
  size_t i = 0;
  while (i < active_count_) {
    if (!active_[i].from_clip || present(active_[i].clip_id, active_[i].destination_id,
                                         active_[i].source_track_id, render_frame)) {
      ++i;
      continue;
    }
    const ActiveNote note = active_[i];
    MidiEvent off;
    off.render_frame = render_frame;
    off.ump = make_midi1_note_off(note.group, note.channel, note.note, 0);
    off.source_track_id = note.source_track_id;
    clear_note_tracking_for_event(note.destination_id, off, note.from_clip, note.clip_id);
    active_[i] = active_[active_count_ - 1];
    --active_count_;
    dispatch(note.destination_id, off);
  }
  size_t p = 0;
  if (runtime_storage_ == nullptr) return;
  while (p < pending_fx_count_) {
    if (!runtime_storage_->pending_fx[p].from_clip ||
        present(runtime_storage_->pending_fx[p].clip_id,
                runtime_storage_->pending_fx[p].destination_id,
                runtime_storage_->pending_fx[p].event.source_track_id,
                runtime_storage_->pending_fx[p].event.render_frame)) {
      ++p;
      continue;
    }
    clear_pending_note_tracking_for_event(runtime_storage_->pending_fx[p]);
    erase_pending(p);
  }
}

void MidiSequencer::process_event(uint32_t destination_id, const MidiEvent& event,
                                  int64_t block_end_frame, bool from_clip,
                                  uint32_t clip_id) noexcept {
  // SysEx bypasses MIDI FX: its borrowed views are valid for this dispatch only.
  if (is_sysex_event(event)) {
    dispatch_transformed(destination_id, event, from_clip, clip_id);
    return;
  }
  DestinationFx* fx = find_midi_fx(destination_id);
  if (fx == nullptr) {
    dispatch_transformed(destination_id, event, from_clip, clip_id);
    return;
  }
  fx->chain.process_chunk(&event, 1, fx->next_input_ordinal++, &fx->buffer, from_clip, clip_id);
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
  const int64_t block_end_frame = numeric::saturating_add<int64_t>(block_start_frame, num_frames);
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
  // Visit every clip event and synthetic clip/loop end in the block, skipping
  // events before @p from_frame. The visitor is allocation-free; process_block
  // uses it first to select the next render frame, then again to dispatch every
  // item at that frame. Each clip's event scan starts at a binary search, and
  // the merge keeps all dispatches monotonic without an audio-thread sort buffer.
  auto visit_scheduled = [&](int64_t from_frame, auto&& visitor) noexcept {
    if (clips == nullptr) return;
    for (const MidiClipSchedule& clip : *clips) {
      if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
        const int64_t loop_len = clip.loop_length_samples;
        const int64_t clip_end_frame =
            clip.length_samples > 0 ? saturating_clip_end(clip) : block_end_frame;
        const int64_t scan_start = std::max(block_start_frame, clip.start_sample);
        const int64_t scan_end = std::min(block_end_frame, clip_end_frame);
        if (scan_start >= scan_end) continue;
        for (int64_t iter_start = scan_start - loop_phase(clip, scan_start); iter_start < scan_end;
             iter_start = numeric::saturating_add(iter_start, loop_len)) {
          const int64_t iter_end = numeric::saturating_add(iter_start, loop_len);
          const auto events_end = clip.events.end();
          for (auto it = first_loop_event_at(clip, iter_start, from_frame); it != events_end;
               ++it) {
            const MidiEvent& event = *it;
            const int64_t local = numeric::saturating_sub(event.render_frame, clip.start_sample);
            if (local < 0) continue;
            if (local >= loop_len) break;
            const int64_t frame = numeric::saturating_add(iter_start, local);
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
      const int64_t clip_end_frame = saturating_clip_end(clip);
      if (finite_one_shot && clip_end_frame <= block_start_frame) continue;
      const auto events_end = clip.events.end();
      for (auto it = first_event_at(clip, from_frame); it != events_end; ++it) {
        const MidiEvent& event = *it;
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

  // Pending MIDI-FX output joins clip output in one timestamp/rank merge, in insertion order.
  auto dispatch_pending_at_rank = [&](int64_t frame, int rank) noexcept {
    if (runtime_storage_ == nullptr) return;
    size_t i = 0;
    while (i < pending_fx_count_) {
      const PendingFxEvent pending = runtime_storage_->pending_fx[i];
      const int64_t pending_frame = std::max(pending.event.render_frame, block_start_frame);
      if (pending_frame != frame || same_time_rank(pending.event.ump) != rank) {
        ++i;
        continue;
      }

      // Remove before dispatch, since a channel-mode reset may clear other pending slots.
      erase_pending(i);
      MidiEvent event = pending.event;
      if (event.render_frame < block_start_frame) event.render_frame = block_start_frame;
      dispatch_transformed(pending.destination_id, event, pending.from_clip, pending.clip_id);
      // The dispatch may have shifted the queue; restart so no same-rank event is skipped.
      i = 0;
    }
  };
  auto dispatch_pending_before = [&](int64_t frame) noexcept {
    if (runtime_storage_ == nullptr) return;
    for (;;) {
      int64_t earliest = frame;
      for (size_t i = 0; i < pending_fx_count_; ++i) {
        const int64_t pending_frame =
            std::max(runtime_storage_->pending_fx[i].event.render_frame, block_start_frame);
        if (pending_frame < earliest && pending_frame < block_end_frame) {
          earliest = pending_frame;
        }
      }
      if (earliest == frame) return;
      for (int rank = 0; rank <= kMaxSameTimeRank; ++rank) {
        dispatch_pending_at_rank(earliest, rank);
      }
    }
  };

  int64_t cursor = block_start_frame;
  while (cursor < block_end_frame) {
    int64_t next_frame = block_end_frame;
    if (runtime_storage_ != nullptr) {
      for (size_t i = 0; i < pending_fx_count_; ++i) {
        const int64_t frame =
            std::max(runtime_storage_->pending_fx[i].event.render_frame, block_start_frame);
        if (frame >= cursor && frame < next_frame) next_frame = frame;
      }
    }
    visit_scheduled(cursor,
                    [&](const MidiClipSchedule&, const MidiEvent*, int64_t frame, bool) noexcept {
                      if (frame >= cursor && frame < next_frame) next_frame = frame;
                    });
    if (next_frame >= block_end_frame) break;

    // Flush earlier carried events, then merge both sources at this frame by same_time_rank.
    dispatch_pending_before(next_frame);
    for (int rank = 0; rank <= kMaxSameTimeRank; ++rank) {
      dispatch_pending_at_rank(next_frame, rank);
      visit_scheduled(next_frame, [&](const MidiClipSchedule& clip, const MidiEvent* event,
                                      int64_t frame, bool clear_pending) noexcept {
        if (frame != next_frame) return;
        const int scheduled_rank = event == nullptr ? 0 : same_time_rank(event->ump);
        if (scheduled_rank != rank) return;
        if (event != nullptr) {
          MidiEvent scheduled = *event;
          scheduled.render_frame = frame;
          scheduled.source_track_id = clip.track_id;
          process_event(clip.destination_id, scheduled, block_end_frame, /*from_clip=*/true,
                        clip.id);
        } else {
          release_notes_for_clip(clip.id, frame, clear_pending);
        }
      });
    }
    cursor = next_frame + 1;
  }

  // Clip-end note releases historically occur exactly at the exclusive block
  // boundary. Keep that contract while leaving pending/generated events at the
  // same frame for the next block.
  if (clips != nullptr) {
    for (const MidiClipSchedule& clip : *clips) {
      if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
        const int64_t clip_end_frame =
            clip.length_samples > 0 ? saturating_clip_end(clip) : block_end_frame;
        if (block_end_frame > clip.start_sample && loop_phase(clip, block_end_frame) == 0 &&
            block_end_frame <= clip_end_frame) {
          release_notes_for_clip(clip.id, block_end_frame, /*clear_pending=*/false);
        }
        if (clip.length_samples > 0 && clip_end_frame == block_end_frame) {
          release_notes_for_clip(clip.id, block_end_frame);
        }
      } else if (clip.loop_mode == MidiLoopMode::kOneShot && clip.length_samples > 0 &&
                 saturating_clip_end(clip) == block_end_frame) {
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
  // The retained table outlives note-offs, so it covers sounding and released channels alike.
  for (const RetainedChannelState& state : retained_channels_) {
    if (!state.active || (single_destination && state.destination_id != destination_id)) {
      continue;
    }
    emit_controller_reset(state.destination_id, state.group, state.channel, render_frame);
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
  if (runtime_storage_ != nullptr) {
    for (DestinationFx& fx : runtime_storage_->midi_fx) {
      if (fx.active) fx.chain.clear_note_tracking();
    }
  }
  clear_retained_channel_states(/*single_destination=*/false, 0);
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
  if (DestinationFx* fx = find_midi_fx(destination_id); fx != nullptr) {
    fx->chain.clear_note_tracking();
  }
  clear_retained_channel_states(/*single_destination=*/true, destination_id);
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
                                 const uint8_t* sysex_payload, size_t sysex_payload_size,
                                 const PreparedMidiSysEx* prepared_sysex) noexcept {
  MidiEvent event;
  event.render_frame = render_frame;
  event.ump = ump;
  event.sysex_payload = sysex_payload;
  event.sysex_payload_size = sysex_payload_size;
  event.prepared_sysex = prepared_sysex;
  process_event(destination_id, event, render_frame + 1, /*from_clip=*/false, 0);
}

void MidiSequencer::BoundaryOffsets::prepare(size_t capacity) {
  const size_t next_capacity = std::max(kCapacity, capacity);
  std::vector<int> next_offsets;
  std::vector<uint8_t> next_seen;
  if (next_capacity > kCapacity) {
    next_offsets.resize(next_capacity);
    next_seen.resize(next_capacity, 0);
  }
  prepared_offsets_.swap(next_offsets);
  prepared_seen_.swap(next_seen);
  inline_seen_.fill(0);
  capacity_ = next_capacity;
  size_ = 0;
  overflowed_ = false;
}

void MidiSequencer::BoundaryOffsets::clear() noexcept {
  uint8_t* const marks = seen();
  const int* const stored = offsets();
  for (size_t i = 0; i < size_; ++i) {
    const int offset = stored[i];
    if (offset >= 0 && static_cast<size_t>(offset) < capacity_) {
      marks[static_cast<size_t>(offset)] = 0;
    }
  }
  size_ = 0;
  overflowed_ = false;
}

void MidiSequencer::BoundaryOffsets::push(int offset) noexcept {
  int* const stored = offsets();
  const bool marked = offset >= 0 && static_cast<size_t>(offset) < capacity_;
  if (marked) {
    if (seen()[static_cast<size_t>(offset)] != 0) return;
  } else {
    for (size_t i = 0; i < size_; ++i) {
      if (stored[i] == offset) return;
    }
  }
  if (size_ >= capacity_) {
    overflowed_ = true;
    return;
  }
  // Mark only stored offsets, so clear() can reset exactly what was set.
  if (marked) seen()[static_cast<size_t>(offset)] = 1;
  size_t pos = 0;
  while (pos < size_ && stored[pos] < offset) {
    ++pos;
  }
  for (size_t i = size_; i > pos; --i) {
    stored[i] = stored[i - 1];
  }
  stored[pos] = offset;
  ++size_;
}

void MidiSequencer::collect_boundaries(int64_t block_start_frame, int num_frames,
                                       BoundaryOffsets* out) const noexcept {
  if (out == nullptr) return;
  out->clear();
  if (num_frames <= 0) return;
  const int64_t block_end_frame = numeric::saturating_add<int64_t>(block_start_frame, num_frames);
  const std::vector<MidiClipSchedule>* clips = clips_.current();
  if (clips == nullptr) return;

  auto push_offset = [out](int offset) noexcept { out->push(offset); };

  for (const MidiClipSchedule& clip : *clips) {
    if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
      const int64_t loop_len = clip.loop_length_samples;
      const int64_t clip_end_frame =
          clip.length_samples > 0 ? saturating_clip_end(clip) : block_end_frame;
      const int64_t scan_start = std::max(block_start_frame, clip.start_sample);
      const int64_t scan_end = std::min(block_end_frame, clip_end_frame);
      if (scan_start >= scan_end) continue;

      for (int64_t iter_start = scan_start - loop_phase(clip, scan_start); iter_start < scan_end;
           iter_start = numeric::saturating_add(iter_start, loop_len)) {
        const int64_t iter_end = numeric::saturating_add(iter_start, loop_len);
        const auto events_end = clip.events.end();
        for (auto it = first_loop_event_at(clip, iter_start, block_start_frame); it != events_end;
             ++it) {
          const MidiEvent& ev = *it;
          const int64_t local = numeric::saturating_sub(ev.render_frame, clip.start_sample);
          if (local < 0) continue;
          if (local >= loop_len) break;
          const int64_t render_frame = numeric::saturating_add(iter_start, local);
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
    const int64_t clip_end_frame = saturating_clip_end(clip);
    const auto events_end = clip.events.end();
    for (auto it = first_event_at(clip, block_start_frame); it != events_end; ++it) {
      const MidiEvent& ev = *it;
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
