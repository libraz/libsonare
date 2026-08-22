#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"

#if defined(SONARE_WITH_ARRANGEMENT)
namespace sonare::engine {

bool RealtimeEngine::bind_midi_cc(uint8_t controller, uint8_t channel, uint32_t param_id,
                                  float min_value, float max_value) noexcept {
  midi::CcBinding binding{};
  binding.cc_number = controller;
  binding.channel = channel;
  binding.param_id = param_id;
  binding.min_value = min_value;
  binding.max_value = max_value;
  return bind_midi_cc(binding);
}

bool RealtimeEngine::bind_midi_cc(const midi::CcBinding& binding) noexcept {
  if (parameter_target_reserved(binding.param_id)) return false;
  try {
    auto next = std::make_shared<midi::CcMap>();
    if (const std::shared_ptr<const midi::CcMap>& current = midi_cc_maps_.control_current()) {
      next->copy_bindings_from(*current);
    }
    if (!next->bind(binding)) return false;
    return midi_cc_maps_.publish(std::shared_ptr<const midi::CcMap>(std::move(next)));
  } catch (...) {
    return false;
  }
}

void RealtimeEngine::clear_midi_cc_bindings() noexcept {
  try {
    auto next = std::make_shared<midi::CcMap>();
    midi_cc_maps_.publish(std::shared_ptr<const midi::CcMap>(std::move(next)));
  } catch (...) {
    // Preserve the current map if a new empty snapshot cannot be allocated.
  }
}

size_t RealtimeEngine::midi_cc_binding_count() const noexcept {
  const std::shared_ptr<const midi::CcMap>& current = midi_cc_maps_.control_current();
  return current ? current->binding_count() : 0;
}

void RealtimeEngine::set_midi_clips(std::vector<midi::MidiClipSchedule> clips) {
  midi_sequencer_.set_midi_clips(std::move(clips));
}

bool RealtimeEngine::push_midi_sysex(uint32_t destination_id, const uint8_t* data, size_t size,
                                     int64_t render_frame) noexcept {
  // CONTROL thread. Copy the SysEx bytes into the next round-robin store slot as
  // a seqlock writer, then enqueue a scalar-only command referencing the slot.
  // The slot generation is a per-slot even/odd sequence: an ODD value marks a
  // write in progress and an EVEN value marks a completed (published) payload.
  // The control thread is the sole writer of a slot's generation, so reading its
  // own last (even) value with relaxed order is safe. The audio-thread reader
  // (apply_command, kMidiSysExImmediate) brackets its payload copy with two
  // acquire loads of the generation and accepts only a stable even value that
  // matches the command's generation, so a slot recycled mid-read (torn payload)
  // or actively being rewritten (odd) is dropped.
  if (data == nullptr || size == 0 || size > kMaxSysExPayloadBytes) return false;
  const uint32_t slot_index = sysex_payload_cursor_ % kSysExPayloadSlots;
  sysex_payload_cursor_++;
  SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
  const uint32_t base = slot.generation.load(std::memory_order_relaxed);  // last even (this thread)
  const uint32_t generation = base + 2u;                                  // even: published value
  slot.generation.store(base + 1u, std::memory_order_relaxed);            // odd: write in progress
  // Order the in-progress mark before the payload writes so a reader can never
  // observe fresh payload bytes still tagged with the previous (even) generation.
  std::atomic_thread_fence(std::memory_order_release);
  slot.store_payload(data, static_cast<uint32_t>(size));
  // Release-store the even (done) generation; it publishes the payload writes to
  // the audio thread's acquire load.
  slot.generation.store(generation, std::memory_order_release);
  // Enqueue the audio-visible command FIRST. The control-thread realise below
  // must not run unless the audio thread will actually adopt the matching
  // channel/EFX state, or a full queue would leave a half-applied SysEx: the new
  // effect chain adopted while the queued channel state never arrives, diverging
  // from an offline bounce. On overflow the staged payload slot is simply left
  // unreferenced (recycled by a later writer) and nothing is realised.
  rt::Command command{};
  command.type = rt::CommandType::kMidiSysExImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i =
      static_cast<int64_t>((static_cast<uint64_t>(generation) << 32) | uint64_t{slot_index});
  if (!push_command(command)) return false;
  // The command is queued: realise any control-thread-built effect of this SysEx
  // (e.g. a GS insertion effect) off the audio thread now, on this control
  // thread; the audio thread swaps the rebuilt chains in wait-free at its next
  // block (a one-block lag relative to the queued state is acceptable).
  // Instruments that do not realise control-thread state (default MidiInstrument)
  // no-op here.
  if (midi::MidiInstrument* instrument = instrument_rack_.get(destination_id)) {
    instrument->on_control_sysex(data, size);
  }
  return true;
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
  struct SinkContext {
    MidiSyncSink* sink;
    int64_t timeline_start;
    int64_t render_start;
  } context{sync_sink, timeline_start_sample, render_start_frame};
  const auto emit = [](void* opaque, int64_t timeline_tick_frame) noexcept {
    auto* state = static_cast<SinkContext*>(opaque);
    const int64_t render_frame =
        state->render_start + (timeline_tick_frame - state->timeline_start);
    state->sink->on_midi_sync_byte(render_frame, midi::kStatusClock);
  };
  bool overflowed = false;
  midi_clock_.generate_clock_block(timeline_start_sample, num_frames, &context, emit, &overflowed);
  if (overflowed) {
    enqueue_error(TelemetryErrorCode::kMidiClockOverflow, render_start_frame, timeline_start_sample,
                  1);
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

void RealtimeEngine::observe_live_cc_for_automation(const midi::Ump& ump) noexcept {
  // The ONE live CC decode in the engine. Every path that can deliver a live
  // controller message -- the queued scalar CC command, the queued raw UMP
  // command, and the engine-owned live input source -- runs through here, so a
  // gesture resolves to the same parameter and the same value whichever one it
  // arrived on.
  //
  // observe_live_cc is the kind-aware decoder: it accumulates 14-bit MSB/LSB
  // pairs and RPN/NRPN selector + Data Entry state per channel, so a
  // high-resolution controller drives its parameter at full precision instead of
  // MSB-only 7 bits, and Data Entry resolves against the selector currently
  // addressed on that channel. The cc_number-only lookup_param / value_to_unit
  // pair cannot do either, which is why no live path calls it any more.
  //
  // AUDIO thread: called from apply_command and from dispatch_live_midi_input,
  // both inside process(). The per-channel accumulator it mutates is owned by
  // that single thread.
  const midi::CcMap* cc_map = midi_cc_maps_.current();
  if (cc_map == nullptr) return;
  uint32_t param_id = 0;
  float mapped_value = 0.0f;
  if (cc_map->observe_live_cc(ump, &param_id, &mapped_value)) {
    automation_.set_parameter(param_id, mapped_value);
  }
}

void RealtimeEngine::dispatch_live_midi_input(int64_t render_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t render_end_frame = render_start_frame + num_frames;
  for (size_t i = 0; i < live_midi_input_count_; ++i) {
    const midi::MidiEvent& event = live_midi_input_events_[i];
    if (event.render_frame < render_start_frame) continue;
    if (event.render_frame >= render_end_frame) break;
    observe_live_cc_for_automation(event.ump);
    midi_sequencer_.inject_event(live_midi_input_destination_id_, event.render_frame, event.ump);
  }
}

void RealtimeEngine::set_midi_instrument(midi::MidiInstrument* instrument) {
  set_midi_instrument(0, instrument);
}

bool RealtimeEngine::set_midi_instrument(uint32_t destination_id,
                                         midi::MidiInstrument* instrument) {
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
  if (previous != instrument) {
    // Retire this destination's automation smoothers. Param ids are per
    // instrument implementation, so carrying a slot across a swap would push the
    // outgoing instrument's value into an unrelated parameter of the incoming
    // one. The host re-resolves and re-drives after a rebind.
    release_instrument_automations(destination_id);
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
  if (!recompute_pdc()) {
    // Compensation could not be reallocated, so the engine has fallen back to
    // none at all and this binding would render misaligned against the clip bus.
    // Undo it and report: leaving it bound while answering false would strand a
    // raw pointer in the rack, which the C-ABI and WASM wrappers free as soon as
    // they see the false.
    instrument_rack_.set(destination_id, nullptr);
    release_instrument_automations(destination_id);
    (void)recompute_pdc();
    return false;
  }
  return true;
}

bool RealtimeEngine::recompute_pdc() {
  // The whole project's reported latency is the slowest bound instrument: every
  // source must be delayed to meet it. Clip audio (zero latency) is delayed by
  // the full total; an instrument that already self-delays by L_i needs only the
  // remaining (total - L_i). After both, all sources coincide at +total. Tracked
  // in Q8.8 so an instrument's sub-sample latency is compensated too.
  const int next_total_q8 = instrument_rack_.max_latency_samples_q8();
  constexpr size_t kSlots = InstrumentRack::kMaxInstruments;
  // Derive the target arrangement first, without touching a single bank: one
  // slot per bound instrument, in rack order, each carrying (total - its own).
  std::array<uint32_t, kSlots> next_instrument_pdc_dest{};
  std::array<int, kSlots> next_delay_q8{};
  size_t next_count = 0;
  instrument_rack_.for_each([&](uint32_t destination_id, midi::MidiInstrument* instrument) {
    if (next_count >= kSlots) return;
    next_instrument_pdc_dest[next_count] = destination_id;
    next_delay_q8[next_count] = next_total_q8 - instrument->latency_samples_q8();
    ++next_count;
  });

  // Carry a bank over whenever the destination it already serves still needs the
  // very same storage shape. A bind names one destination, but every source's
  // compensation is derived from the same maximum, so most binds leave the other
  // banks' shapes untouched -- and rebuilding one zero-fills it, which is a
  // dropout the length of that compensation on an instrument the bind never
  // mentioned. Matching by destination id rather than by slot keeps that true
  // when an unbind packs the remaining instruments down into lower slots.
  constexpr size_t kNoReuse = kSlots;
  std::array<size_t, kSlots> reuse_from{};
  reuse_from.fill(kNoReuse);
  for (size_t slot = 0; slot < next_count; ++slot) {
    for (size_t live = 0; live < pdc_instrument_count_ && live < kSlots; ++live) {
      if (instrument_pdc_dest_[live] != next_instrument_pdc_dest[slot]) continue;
      if (instrument_pdc_delays_[live].matches_storage(prepared_channels_, next_delay_q8[slot])) {
        reuse_from[slot] = live;
      }
      break;
    }
  }

  // Build the replacements the shape changes genuinely require off to the side.
  // In particular, do not configure the live clip bank before an instrument bank
  // has succeeded: an allocation failure must leave the old delay/count pair
  // internally consistent and still usable.
  const bool reuse_clip = clip_pdc_delay_.matches_storage(prepared_channels_, next_total_q8);
  ChannelDelay<kMaxAudioChannels> next_clip_pdc_delay;
  std::array<ChannelDelay<kMaxAudioChannels>, kSlots> next_instrument_pdc_delays{};
  bool configured = reuse_clip || next_clip_pdc_delay.configure(prepared_channels_, next_total_q8);
  for (size_t slot = 0; slot < next_count && configured; ++slot) {
    if (reuse_from[slot] != kNoReuse) continue;
    configured =
        next_instrument_pdc_delays[slot].configure(prepared_channels_, next_delay_q8[slot]);
  }
  if (!configured) {
    // Fail closed if any replacement allocation failed. configure(0, 0) only
    // swaps empty vectors, so this reclamation path remains non-allocating even
    // when the control-thread allocation failure is being deliberately tested.
    clip_pdc_delay_.configure(0, 0);
    for (ChannelDelay<kMaxAudioChannels>& delay : instrument_pdc_delays_) {
      delay.configure(0, 0);
    }
    pdc_total_q8_ = 0;
    pdc_instrument_count_ = 0;
    instrument_pdc_dest_.fill(0);
    update_reported_graph_latency();
    return false;
  }
  // Commit. Allocation-free from here: a carried bank is moved by swap and then
  // only has its scalars republished (configure() takes its no-op path for a
  // shape it already holds), so its delay history survives the recompute.
  if (reuse_clip) {
    clip_pdc_delay_.configure(prepared_channels_, next_total_q8);
  } else {
    next_clip_pdc_delay.swap(clip_pdc_delay_);
  }
  // Lift every carried bank out of the live array before anything is written
  // back into it, so a slot reshuffle cannot overwrite one that has not moved.
  for (size_t slot = 0; slot < next_count; ++slot) {
    if (reuse_from[slot] == kNoReuse) continue;
    next_instrument_pdc_delays[slot].swap(instrument_pdc_delays_[reuse_from[slot]]);
  }
  for (size_t slot = 0; slot < instrument_pdc_delays_.size(); ++slot) {
    next_instrument_pdc_delays[slot].swap(instrument_pdc_delays_[slot]);
  }
  for (size_t slot = 0; slot < next_count; ++slot) {
    if (reuse_from[slot] == kNoReuse) continue;
    instrument_pdc_delays_[slot].configure(prepared_channels_, next_delay_q8[slot]);
  }
  instrument_pdc_dest_ = next_instrument_pdc_dest;
  pdc_total_q8_ = next_total_q8;
  pdc_instrument_count_ = next_count;
  // Surface the applied compensation as the engine's graph latency so transport
  // telemetry (audible_timeline_sample) reflects the real output delay.
  update_reported_graph_latency();
  return true;
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
