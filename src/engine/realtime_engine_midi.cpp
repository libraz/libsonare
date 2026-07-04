#include <cstring>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"

#if defined(SONARE_WITH_ARRANGEMENT)
namespace sonare::engine {

void RealtimeEngine::set_midi_clips(std::vector<midi::MidiClipSchedule> clips) {
  midi_sequencer_.set_midi_clips(std::move(clips));
}

bool RealtimeEngine::push_midi_sysex(uint32_t destination_id, const uint8_t* data, size_t size,
                                     int64_t render_frame) noexcept {
  // CONTROL thread. Copy the SysEx bytes into the next round-robin store slot,
  // bump its generation, then enqueue a scalar-only command referencing the
  // slot. push_command's release publishes the freshly written slot bytes and
  // generation to the audio thread; apply_command validates the generation
  // before viewing the bytes (see kMidiSysExImmediate).
  if (data == nullptr || size == 0 || size > kMaxSysExPayloadBytes) return false;
  const uint32_t slot_index = sysex_payload_cursor_ % kSysExPayloadSlots;
  sysex_payload_cursor_++;
  SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
  std::memcpy(slot.bytes.data(), data, size);
  slot.size = static_cast<uint32_t>(size);
  const uint32_t generation = slot.generation.load(std::memory_order_relaxed) + 1u;
  slot.generation.store(generation, std::memory_order_relaxed);
  // Realise any control-thread-built effect of this SysEx (e.g. a GS insertion
  // effect) off the audio thread now, on this control thread; the audio thread
  // swaps the rebuilt chains in wait-free at its next block. The audio-visible
  // channel/EFX state is still delivered by the queued command below. Instruments
  // that do not realise control-thread state (default MidiInstrument) no-op here.
  if (midi::MidiInstrument* instrument = instrument_rack_.get(destination_id)) {
    instrument->on_control_sysex(data, size);
  }
  rt::Command command{};
  command.type = rt::CommandType::kMidiSysExImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i =
      static_cast<int64_t>((static_cast<uint64_t>(generation) << 32) | uint64_t{slot_index});
  return push_command(command);
}

bool RealtimeEngine::set_midi_fx(uint32_t destination_id, const midi::MidiFxChain& chain) noexcept {
  return midi_sequencer_.set_midi_fx(destination_id, chain, transport_.render_frame());
}

void RealtimeEngine::clear_midi_fx(uint32_t destination_id) noexcept {
  midi_sequencer_.clear_midi_fx(destination_id);
}

void RealtimeEngine::emit_midi_transport_command(uint8_t status, int64_t render_frame) noexcept {
  MidiSyncSink* sync_sink = midi_sync_sink_.load(std::memory_order_acquire);
  if (sync_sink == nullptr) return;
  uint8_t byte = 0;
  if (midi::encode_transport_command(status, &byte, 1) != 1) return;
  sync_sink->on_midi_sync_byte(render_frame, byte);
}

void RealtimeEngine::emit_midi_clock_block(int64_t timeline_start_sample,
                                           int64_t render_start_frame, int num_frames) noexcept {
  MidiSyncSink* sync_sink = midi_sync_sink_.load(std::memory_order_acquire);
  if (sync_sink == nullptr || num_frames <= 0) return;
  const int64_t block_end_sample = timeline_start_sample + num_frames;
  for (int64_t tick = midi_clock_.first_tick_at_or_after(timeline_start_sample);
       midi_clock_.frame_of_tick(tick) < block_end_sample; ++tick) {
    const int64_t timeline_tick_frame = midi_clock_.frame_of_tick(tick);
    if (timeline_tick_frame < timeline_start_sample) continue;
    const int64_t render_frame = render_start_frame + (timeline_tick_frame - timeline_start_sample);
    sync_sink->on_midi_sync_byte(render_frame, midi::kStatusClock);
  }
}

bool RealtimeEngine::set_midi_destination_external(uint32_t destination_id,
                                                   bool external) noexcept {
  return midi_dispatch_sink_.set_external(destination_id, external);
}

size_t RealtimeEngine::drain_external_midi(host::ExternalMidiRecord* out,
                                           size_t capacity) noexcept {
  return external_midi_queue_.drain(out, capacity);
}

void RealtimeEngine::set_external_midi_clock_enabled(bool enabled) noexcept {
  // Enabling registers the engine-internal sync sink so emit_midi_clock_block /
  // emit_midi_transport_command funnel clock/transport bytes into the external
  // queue; disabling clears it (and any other registered sync sink).
  set_midi_sync_sink(enabled ? &external_clock_sync_sink_ : nullptr);
}

void RealtimeEngine::dispatch_live_midi_input(int64_t render_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t render_end_frame = render_start_frame + num_frames;
  for (size_t i = 0; i < live_midi_input_count_; ++i) {
    const midi::MidiEvent& event = live_midi_input_events_[i];
    if (event.render_frame < render_start_frame) continue;
    if (event.render_frame >= render_end_frame) break;
    // Kind-aware live decode: accumulates 14-bit MSB/LSB and RPN/NRPN selector +
    // Data Entry state so high-resolution controllers drive the parameter at full
    // precision instead of MSB-only 7 bits (see CcMap::observe_live_cc).
    uint32_t param_id = 0;
    float mapped_value = 0.0f;
    if (midi_cc_map_.observe_live_cc(event.ump, &param_id, &mapped_value)) {
      automation_.set_parameter(param_id, mapped_value);
    }
    midi_sequencer_.inject_event(live_midi_input_destination_id_, event.render_frame, event.ump);
  }
}

void RealtimeEngine::set_midi_instrument(midi::MidiInstrument* instrument) noexcept {
  set_midi_instrument(0, instrument);
}

bool RealtimeEngine::set_midi_instrument(uint32_t destination_id,
                                         midi::MidiInstrument* instrument) noexcept {
  // Hang-note safety on swap/clear: if this destination currently has a bound
  // instrument that is about to be replaced or removed, release every note
  // sounding on it first. The note-offs route through the rack to the OUTGOING
  // instrument (still bound at this point) before the binding changes, so it
  // does not leave a hanging note. set_midi_instrument is control-thread only
  // and called between blocks, matching the sequencer's mutation contract.
  midi::MidiInstrument* const previous = instrument_rack_.get(destination_id);
  if (previous != nullptr && previous != instrument) {
    midi_sequencer_.all_notes_off_for_destination(destination_id, transport_.render_frame());
  }
  if (!instrument_rack_.set(destination_id, instrument)) {
    return false;  // rack full: leave existing bindings untouched
  }
  // The sequencer's sink is the rack itself (set in prepare); no per-instrument
  // sink wiring is needed. Prepare the freshly-registered instrument to the
  // engine's sample rate / block size. prepare() may allocate, so this stays a
  // control-thread operation.
  if (instrument != nullptr && max_block_size_ > 0) {
    instrument->prepare(sample_rate_, max_block_size_);
  }
  // The bound set (and thus the maximum instrument latency) changed: refresh the
  // PDC delays so clip + instrument audio stays phase-aligned. Control-thread
  // only, matching the delay lines' reallocation contract.
  recompute_pdc();
  return true;
}

void RealtimeEngine::recompute_pdc() noexcept {
  // The whole project's reported latency is the slowest bound instrument: every
  // source must be delayed to meet it. Clip audio (zero latency) is delayed by
  // the full total; an instrument that already self-delays by L_i needs only the
  // remaining (total - L_i). After both, all sources coincide at +total. Tracked
  // in Q8.8 so an instrument's sub-sample latency is compensated too.
  pdc_total_q8_ = instrument_rack_.max_latency_samples_q8();
  clip_pdc_delay_.set_delay_q8(pdc_total_q8_);
  pdc_instrument_count_ = 0;
  instrument_rack_.for_each([&](uint32_t destination_id, midi::MidiInstrument* instrument) {
    if (pdc_instrument_count_ >= instrument_pdc_delays_.size()) return;
    const size_t slot = pdc_instrument_count_++;
    instrument_pdc_dest_[slot] = destination_id;
    instrument_pdc_delays_[slot].set_delay_q8(pdc_total_q8_ - instrument->latency_samples_q8());
  });
  // Surface the applied compensation as the engine's graph latency so transport
  // telemetry (audible_timeline_sample) reflects the real output delay.
  update_reported_graph_latency();
}

void RealtimeEngine::flush_pdc_delays() noexcept {
  if (pdc_total_q8_ > 0) {
    clip_pdc_delay_.reset();
    for (size_t i = 0; i < pdc_instrument_count_; ++i) {
      instrument_pdc_delays_[i].reset();
    }
  }
#if defined(SONARE_WITH_MIXING)
  track_mixer_runtime_.flush_pdc_delays();
#endif
}

}  // namespace sonare::engine
#endif
