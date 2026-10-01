#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"
#include "util/exception.h"

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

void RealtimeEngine::reclaim_released_sysex_slots() noexcept {
  // Only CONTROL destroys a token, once AUDIO has published released == generation.
  for (SysExPayloadSlot& slot : sysex_payload_slots_) {
    const uint32_t generation = slot.generation.load(std::memory_order_acquire);
    if (slot.released.load(std::memory_order_acquire) != generation) continue;
    slot.prepared.reset();
    slot.destination_id = 0;
    slot.accepted_order = 0;
  }
}

bool RealtimeEngine::prepare_instrument_sysex(
    midi::MidiInstrument* instrument, const uint8_t* payload, size_t size,
    std::shared_ptr<const midi::PreparedMidiSysEx>& prepared) {
  prepared.reset();
  if (instrument == nullptr) return true;
  try {
    return instrument->prepare_sysex(payload, size, prepared);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return false;
  }
}

midi::MidiSysExPreparer RealtimeEngine::rack_sysex_preparer() const {
  return [this](uint32_t destination, const uint8_t* payload, size_t size,
                std::shared_ptr<const midi::PreparedMidiSysEx>& prepared) {
    return prepare_instrument_sysex(instrument_rack_.get(destination), payload, size, prepared);
  };
}

void RealtimeEngine::set_midi_clips(std::vector<midi::MidiClipSchedule> clips) {
  reclaim_released_sysex_slots();
  // own_sysex_payloads stages a complete bank, so a failure publishes nothing.
  midi::MidiSysExPayloadError error = midi::MidiSysExPayloadError::kNone;
  if (!midi::own_sysex_payloads(clips, &error, rack_sysex_preparer())) {
    throw SonareException(error == midi::MidiSysExPayloadError::kOutOfMemory
                              ? ErrorCode::OutOfMemory
                              : ErrorCode::InvalidParameter,
                          midi::describe(error));
  }
  // The sequencer's own ownership pass keeps each event's bank-owned token.
  midi_sequencer_.set_midi_clips(std::move(clips));
}

bool RealtimeEngine::push_midi_sysex(uint32_t destination_id, const uint8_t* data, size_t size,
                                     int64_t render_frame) noexcept {
  // CONTROL thread. Copy the SysEx bytes into the next store slot as a seqlock
  // writer, then enqueue a scalar-only command referencing the slot.
  // The slot generation is a per-slot even/odd sequence: an ODD value marks a
  // write in progress and an EVEN value marks a completed (published) payload.
  // The control thread is the sole writer of a slot's generation, so reading its
  // own last (even) value with relaxed order is safe. The audio-thread reader
  // (apply_command, kMidiSysExImmediate) brackets its payload copy with two
  // acquire loads of the generation and accepts only a stable even value that
  // matches the command's generation, so a slot recycled mid-read (torn payload)
  // or actively being rewritten (odd) is dropped.
  last_midi_sysex_push_status_.store(MidiSysExPushStatus::kNotAttempted, std::memory_order_relaxed);
  if (data == nullptr || size == 0 || size > kMaxSysExPayloadBytes) {
    last_midi_sysex_push_status_.store(MidiSysExPushStatus::kInvalidPayload,
                                       std::memory_order_relaxed);
    return false;
  }
  reclaim_released_sysex_slots();
  // The slot keeps the token alive until AUDIO releases its generation.
  std::shared_ptr<const midi::PreparedMidiSysEx> prepared;
  midi::MidiInstrument* instrument = instrument_rack_.get(destination_id);
  try {
    if (!prepare_instrument_sysex(instrument, data, size, prepared)) {
      last_midi_sysex_push_status_.store(MidiSysExPushStatus::kPreparationFailed,
                                         std::memory_order_relaxed);
      return false;
    }
  } catch (...) {
    last_midi_sysex_push_status_.store(MidiSysExPushStatus::kOutOfMemory,
                                       std::memory_order_relaxed);
    return false;
  }
  const uint32_t cursor = sysex_payload_cursor_;
  uint32_t slot_index = 0;
  uint32_t base = 0;
  size_t probe = 0;
  for (; probe < kSysExPayloadSlots; ++probe) {
    const uint32_t candidate_index =
        static_cast<uint32_t>((static_cast<size_t>(cursor) + probe) % kSysExPayloadSlots);
    SysExPayloadSlot& candidate = sysex_payload_slots_[candidate_index];
    const uint32_t candidate_base = candidate.generation.load(std::memory_order_relaxed);
    // Never recycle an in-flight payload; scan past a still-pending future slot.
    if (candidate.released.load(std::memory_order_acquire) == candidate_base) {
      slot_index = candidate_index;
      base = candidate_base;
      break;
    }
  }
  if (probe == kSysExPayloadSlots) {
    last_midi_sysex_push_status_.store(MidiSysExPushStatus::kPayloadSlotsFull,
                                       std::memory_order_relaxed);
    return false;
  }
  SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
  sysex_payload_cursor_ += static_cast<uint32_t>(probe + 1);
  const uint32_t generation = base + 2u;                        // even: published value
  slot.generation.store(base + 1u, std::memory_order_relaxed);  // odd: write in progress
  slot.accepted_order = 0;
  // Order the in-progress mark before the payload writes so a reader can never
  // observe fresh payload bytes still tagged with the previous (even) generation.
  std::atomic_thread_fence(std::memory_order_release);
  slot.prepared = std::move(prepared);
  slot.destination_id = destination_id;
  slot.store_payload(data, static_cast<uint32_t>(size));
  // Release-store the even (done) generation; it publishes the payload writes to
  // the audio thread's acquire load.
  slot.generation.store(generation, std::memory_order_release);
  // Enqueue the audio-visible command FIRST. The control-thread realise below
  // must not run unless the audio thread will actually adopt the matching
  // channel/EFX state, or a full queue would leave a half-applied SysEx: the new
  // effect chain adopted while the queued channel state never arrives, diverging
  // from an offline bounce. On overflow the staged payload slot is handed back
  // immediately and nothing is realised.
  rt::Command command{};
  command.type = rt::CommandType::kMidiSysExImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i =
      static_cast<int64_t>((static_cast<uint64_t>(generation) << 32) | uint64_t{slot_index});
  if (!push_command(command)) {
    // Never queued, so the audio thread will never hand it back.
    slot.prepared.reset();
    slot.destination_id = 0;
    slot.accepted_order = 0;
    slot.released.store(generation, std::memory_order_release);
    last_midi_sysex_push_status_.store(MidiSysExPushStatus::kQueueFull, std::memory_order_relaxed);
    return false;
  }
  // Queueing is the acceptance point; scheduled clip SysEx has no such callback.
  uint64_t accepted_order = ++sysex_accepted_order_;
  if (accepted_order == 0) accepted_order = ++sysex_accepted_order_;
  slot.accepted_order = accepted_order;
  if (instrument != nullptr) {
    instrument->on_prepared_sysex_accepted(data, size, slot.prepared.get());
  }
  last_midi_sysex_push_status_.store(MidiSysExPushStatus::kAccepted, std::memory_order_relaxed);
  return true;
}

namespace {

// Types that address the endpoint rather than a destination's instrument: the
// live path accepts them and discards them at the destination.
bool is_endpoint_ump_type(midi::UmpMessageType type) noexcept {
  return type == midi::UmpMessageType::kUtility || type == midi::UmpMessageType::kFlexData ||
         type == midi::UmpMessageType::kStream;
}

constexpr uint64_t discard_counter_key(uint32_t destination_id) noexcept {
  return (uint64_t{1} << 32) | destination_id;
}

}  // namespace

bool RealtimeEngine::is_pushable_midi_ump(const uint32_t* words, size_t count) noexcept {
  if (words == nullptr || count == 0 || count > 4) return false;
  if (count != midi::ump_word_count_for_word0(words[0])) return false;
  const auto type = static_cast<midi::UmpMessageType>((words[0] >> 28) & 0x0Fu);
  return type != midi::UmpMessageType::kData64 && type != midi::UmpMessageType::kData128;
}

MidiUmpPushResult RealtimeEngine::push_midi_ump(uint32_t destination_id, const uint32_t* words,
                                                size_t count, int64_t render_frame) noexcept {
  // CONTROL thread.
  if (!is_pushable_midi_ump(words, count)) return MidiUmpPushResult::kInvalidMessage;
  rt::Command command{};
  command.target_id = destination_id;
  command.sample_time = render_frame;
  if (count == 1) {
    command.type = rt::CommandType::kMidiUmpImmediate;
    command.arg.i = static_cast<int64_t>(words[0]);
    return push_command(command) ? MidiUmpPushResult::kQueued : MidiUmpPushResult::kQueueFull;
  }
  const uint32_t slot_index = ump_slot_cursor_ % kMidiUmpSlots;
  UmpSlot& slot = ump_slots_[slot_index];
  // The control thread is the sole writer of `generation`, so its own last
  // (even) value reads back relaxed. The acquire on `released` orders the audio
  // thread's reads of the previous payload before the rewrite below.
  const uint32_t base = slot.generation.load(std::memory_order_relaxed);
  if (slot.released.load(std::memory_order_acquire) != base) {
    ump_slot_overflow_count_.fetch_add(1, std::memory_order_relaxed);
    return MidiUmpPushResult::kSlotsFull;
  }
  ump_slot_cursor_++;
  const uint32_t generation = base + 2u;
  slot.generation.store(base + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (size_t i = 0; i < slot.words.size(); ++i) {
    slot.words[i].store(i < count ? words[i] : 0u, std::memory_order_relaxed);
  }
  slot.generation.store(generation, std::memory_order_release);
  command.type = rt::CommandType::kMidiUmpSlotImmediate;
  command.arg.i =
      static_cast<int64_t>((static_cast<uint64_t>(generation) << 32) | uint64_t{slot_index});
  if (!push_command(command)) {
    // Never queued, so the audio thread will never hand it back.
    slot.released.store(generation, std::memory_order_release);
    return MidiUmpPushResult::kQueueFull;
  }
  return MidiUmpPushResult::kQueued;
}

uint32_t RealtimeEngine::midi_ump_discarded_count(uint32_t destination_id) const noexcept {
  const uint64_t key = discard_counter_key(destination_id);
  for (const UmpDiscardCounter& counter : ump_discard_counters_) {
    if (counter.key.load(std::memory_order_acquire) == key) {
      return counter.count.load(std::memory_order_relaxed);
    }
  }
  return 0;
}

void RealtimeEngine::deliver_live_ump(uint32_t destination_id, int64_t render_frame,
                                      const midi::Ump& ump) noexcept {
  if (is_endpoint_ump_type(ump.message_type())) {
    ump_discarded_total_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t key = discard_counter_key(destination_id);
    for (UmpDiscardCounter& counter : ump_discard_counters_) {
      const uint64_t current = counter.key.load(std::memory_order_relaxed);
      if (current == 0) counter.key.store(key, std::memory_order_release);
      if (current == 0 || current == key) {
        counter.count.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    return;
  }
  observe_live_cc_for_automation(ump);
  midi_sequencer_.inject_event(destination_id, render_frame, ump);
}

void RealtimeEngine::release_midi_ump_slot(const rt::Command& command) noexcept {
  if (command.type != rt::CommandType::kMidiUmpSlotImmediate) return;
  const uint64_t packed = static_cast<uint64_t>(command.arg.i);
  const uint64_t slot_index = packed & 0xFFFFFFFFu;
  const auto generation = static_cast<uint32_t>(packed >> 32);
  if (slot_index >= ump_slots_.size()) return;
  UmpSlot& slot = ump_slots_[slot_index];
  // Only the command that owns the slot's current payload may hand it back.
  if (slot.generation.load(std::memory_order_acquire) != generation) return;
  if (slot.released.load(std::memory_order_relaxed) == generation) return;
  slot.released.store(generation, std::memory_order_release);
}

void RealtimeEngine::release_midi_sysex_slot(const rt::Command& command) noexcept {
  if (command.type != rt::CommandType::kMidiSysExImmediate) return;
  const uint64_t packed = static_cast<uint64_t>(command.arg.i);
  const uint64_t slot_index = packed & 0xFFFFFFFFu;
  const auto generation = static_cast<uint32_t>(packed >> 32);
  if (slot_index >= sysex_payload_slots_.size()) return;
  SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
  // Only the command that owns the slot's current payload may hand it back.
  if (slot.generation.load(std::memory_order_acquire) != generation) return;
  if (slot.released.load(std::memory_order_relaxed) == generation) return;
  slot.released.store(generation, std::memory_order_release);
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
    deliver_live_ump(live_midi_input_destination_id_, event.render_frame, event.ump);
  }
}

void RealtimeEngine::set_midi_instrument(midi::MidiInstrument* instrument) {
  set_midi_instrument(0, instrument);
}

bool RealtimeEngine::set_midi_instrument(uint32_t destination_id,
                                         midi::MidiInstrument* instrument) {
  const auto report = [this](MidiInstrumentBindStatus status) {
    last_midi_instrument_bind_status_.store(status, std::memory_order_relaxed);
    return status == MidiInstrumentBindStatus::kBound;
  };
  midi::MidiInstrument* const previous = instrument_rack_.get(destination_id);
  if (previous == instrument) return report(MidiInstrumentBindStatus::kBound);
  reclaim_released_sysex_slots();
  if (previous == nullptr && instrument != nullptr &&
      instrument_rack_.size() >= InstrumentRack::kMaxInstruments) {
    return report(MidiInstrumentBindStatus::kRackFull);
  }

  // One instance holds one destination's voice state; refuse a second binding.
  bool already_bound_elsewhere = false;
  if (instrument != nullptr) {
    instrument_rack_.for_each(
        [&](uint32_t bound_destination, midi::MidiInstrument* bound_instrument) {
          if (bound_destination != destination_id && bound_instrument == instrument) {
            already_bound_elsewhere = true;
          }
        });
  }
  if (already_bound_elsewhere) return report(MidiInstrumentBindStatus::kAlreadyBoundElsewhere);

  // Prepared SysEx tokens are only valid against the prepared instrument format.
  try {
    if (instrument != nullptr && max_block_size_ > 0) {
      instrument->prepare(sample_rate_, max_block_size_);
    }
  } catch (const std::bad_alloc&) {
    return report(MidiInstrumentBindStatus::kOutOfMemory);
  } catch (...) {
    return report(MidiInstrumentBindStatus::kPreparationFailed);
  }

  struct PendingTokenUpdate {
    size_t slot_index = 0;
    uint32_t generation = 0;
    uint64_t accepted_order = 0;
    std::shared_ptr<const midi::PreparedMidiSysEx> prepared;
  };
  std::vector<midi::MidiClipSchedule> next_clips;
  std::vector<PendingTokenUpdate> pending_updates;
  PreparedPdc next_pdc;

  try {
    const auto& control_clips = midi_sequencer_.control_clips();
    next_clips = control_clips != nullptr ? *control_clips : std::vector<midi::MidiClipSchedule>{};

    // Other destinations keep their tokens, keyed by the bank-owned payload span.
    std::unordered_map<const uint8_t*, std::shared_ptr<const midi::PreparedMidiSysEx>>
        existing_prepared;
    std::unordered_set<const midi::MidiSysExPayloadBank*> seen_banks;
    for (const midi::MidiClipSchedule& schedule : next_clips) {
      const midi::MidiSysExPayloadBank* bank = schedule.sysex_payload_bank.get();
      if (bank == nullptr || !seen_banks.insert(bank).second) continue;
      for (size_t i = 0; i < bank->payloads.size() && i < bank->prepared_operations.size(); ++i) {
        existing_prepared.emplace(bank->payloads[i].data(), bank->prepared_operations[i]);
      }
    }

    const midi::MidiSysExPreparer prepare =
        [&](uint32_t destination, const uint8_t* payload, size_t size,
            std::shared_ptr<const midi::PreparedMidiSysEx>& prepared) {
          if (destination == destination_id) {
            return prepare_instrument_sysex(instrument, payload, size, prepared);
          }
          prepared.reset();
          const auto existing = existing_prepared.find(payload);
          if (existing != existing_prepared.end()) prepared = existing->second;
          return true;
        };
    midi::MidiSysExPayloadError clip_error = midi::MidiSysExPayloadError::kNone;
    if (!midi::own_sysex_payloads(next_clips, &clip_error, prepare)) {
      return report(clip_error == midi::MidiSysExPayloadError::kOutOfMemory
                        ? MidiInstrumentBindStatus::kOutOfMemory
                        : MidiInstrumentBindStatus::kPreparationFailed);
    }

    std::array<uint8_t, kMaxSysExPayloadBytes> payload{};
    for (size_t slot_index = 0; slot_index < sysex_payload_slots_.size(); ++slot_index) {
      SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
      const uint32_t generation = slot.generation.load(std::memory_order_acquire);
      const uint32_t released = slot.released.load(std::memory_order_acquire);
      if (generation == released) continue;
      if ((generation & 1u) != 0u || slot.destination_id != destination_id) continue;
      const uint32_t payload_size = slot.load_payload(payload);
      if (payload_size == 0) return report(MidiInstrumentBindStatus::kPreparationFailed);
      std::shared_ptr<const midi::PreparedMidiSysEx> prepared;
      if (!prepare_instrument_sysex(instrument, payload.data(), payload_size, prepared)) {
        return report(MidiInstrumentBindStatus::kPreparationFailed);
      }
      pending_updates.push_back({slot_index, generation, slot.accepted_order, std::move(prepared)});
    }

    std::sort(pending_updates.begin(), pending_updates.end(),
              [](const PendingTokenUpdate& left, const PendingTokenUpdate& right) {
                return left.accepted_order < right.accepted_order;
              });

    // Stage the latency banks too, so a failure leaves the old PDC and binding.
    if (!prepare_pdc_for(true, destination_id, instrument, next_pdc)) {
      return report(MidiInstrumentBindStatus::kOutOfMemory);
    }
  } catch (const std::bad_alloc&) {
    return report(MidiInstrumentBindStatus::kOutOfMemory);
  } catch (...) {
    return report(MidiInstrumentBindStatus::kPreparationFailed);
  }

  try {
    // Last fallible step: publish while the old rack is still live.
    midi_sequencer_.set_midi_clips(std::move(next_clips));
  } catch (const SonareException& e) {
    return report(e.code() == ErrorCode::OutOfMemory
                      ? MidiInstrumentBindStatus::kOutOfMemory
                      : MidiInstrumentBindStatus::kPreparationFailed);
  } catch (...) {
    return report(MidiInstrumentBindStatus::kOutOfMemory);
  }

  // From here every step commits; release notes through the outgoing instrument first.
  if (previous != nullptr) {
    midi_sequencer_.all_notes_off_for_destination(destination_id, transport_.render_frame());
  }
  if (!instrument_rack_.set(destination_id, instrument)) {
    return report(MidiInstrumentBindStatus::kRackFull);
  }
  commit_pdc(next_pdc);
  // Adopt now so a full hand-off ring cannot strand the prepared bank as pending.
  midi_sequencer_.acquire_midi_clips_control_quiescent();

  // Token replacement is CONTROL-only; AUDIO release never destroys a token.
  for (PendingTokenUpdate& update : pending_updates) {
    SysExPayloadSlot& slot = sysex_payload_slots_[update.slot_index];
    if (slot.generation.load(std::memory_order_acquire) == update.generation &&
        slot.released.load(std::memory_order_acquire) != update.generation) {
      slot.prepared = std::move(update.prepared);
    }
  }
  if (instrument != nullptr) {
    std::array<uint8_t, kMaxSysExPayloadBytes> payload{};
    for (const PendingTokenUpdate& update : pending_updates) {
      SysExPayloadSlot& slot = sysex_payload_slots_[update.slot_index];
      if (slot.generation.load(std::memory_order_acquire) != update.generation ||
          slot.released.load(std::memory_order_acquire) == update.generation) {
        continue;
      }
      if (update.accepted_order == 0 || slot.prepared != nullptr) continue;
      const uint32_t payload_size = slot.load_payload(payload);
      if (payload_size != 0) {
        instrument->on_prepared_sysex_accepted(payload.data(), payload_size, nullptr);
      }
    }
  }
  release_instrument_automations(destination_id);
  return report(MidiInstrumentBindStatus::kBound);
}

bool RealtimeEngine::prepare_pdc_for(bool replace_destination, uint32_t destination_id,
                                     midi::MidiInstrument* replacement,
                                     PreparedPdc& out) const noexcept {
  constexpr size_t kSlots = PreparedPdc::kSlots;
  constexpr size_t kNoReuse = kSlots;
  std::array<int, kSlots> next_delay_q8{};
  size_t next_count = 0;
  bool replaced = false;

  auto append = [&](uint32_t id, midi::MidiInstrument* instrument) {
    if (instrument == nullptr || next_count >= kSlots) return;
    out.destinations[next_count] = id;
    next_delay_q8[next_count] = instrument->latency_samples_q8();
    out.total_q8 = std::max(out.total_q8, next_delay_q8[next_count]);
    ++next_count;
  };
  instrument_rack_.for_each([&](uint32_t id, midi::MidiInstrument* instrument) {
    if (replace_destination && id == destination_id) {
      replaced = true;
      instrument = replacement;
    }
    append(id, instrument);
  });
  if (replace_destination && !replaced) append(destination_id, replacement);
  out.count = next_count;
  out.reuse_from.fill(kNoReuse);
  out.reuse_clip = clip_pdc_delay_.matches_storage(prepared_channels_, out.total_q8);

  bool configured = out.reuse_clip || out.clip.configure(prepared_channels_, out.total_q8);
  for (size_t slot = 0; slot < next_count && configured; ++slot) {
    for (size_t live = 0; live < pdc_instrument_count_ && live < kSlots; ++live) {
      if (instrument_pdc_dest_[live] != out.destinations[slot]) continue;
      if (instrument_pdc_delays_[live].matches_storage(prepared_channels_,
                                                       out.total_q8 - next_delay_q8[slot])) {
        out.reuse_from[slot] = live;
      }
      break;
    }
    if (out.reuse_from[slot] == kNoReuse) {
      configured =
          out.instruments[slot].configure(prepared_channels_, out.total_q8 - next_delay_q8[slot]);
    }
  }
  return configured;
}

void RealtimeEngine::commit_pdc(PreparedPdc& prepared) noexcept {
  constexpr size_t kSlots = PreparedPdc::kSlots;
  constexpr size_t kNoReuse = kSlots;
  if (prepared.reuse_clip) {
    (void)clip_pdc_delay_.configure(prepared_channels_, prepared.total_q8);
  } else {
    prepared.clip.swap(clip_pdc_delay_);
  }

  // Carry each kept bank into its staging slot so history follows its destination.
  for (size_t slot = 0; slot < prepared.count; ++slot) {
    if (prepared.reuse_from[slot] != kNoReuse) {
      prepared.instruments[slot].swap(instrument_pdc_delays_[prepared.reuse_from[slot]]);
    }
  }
  for (size_t live = 0; live < kSlots; ++live) {
    bool carried = false;
    for (size_t slot = 0; slot < prepared.count; ++slot) {
      if (prepared.reuse_from[slot] == live) {
        carried = true;
        break;
      }
    }
    if (!carried) {
      ChannelDelay<kMaxAudioChannels> empty;
      empty.swap(instrument_pdc_delays_[live]);
    }
  }
  for (size_t slot = 0; slot < prepared.count; ++slot) {
    prepared.instruments[slot].swap(instrument_pdc_delays_[slot]);
  }

  instrument_pdc_dest_ = prepared.destinations;
  pdc_total_q8_ = prepared.total_q8;
  pdc_instrument_count_ = prepared.count;
  update_reported_graph_latency();
}

bool RealtimeEngine::recompute_pdc() {
  // Stage every bank first; a failed recompute leaves the audible PDC untouched.
  PreparedPdc prepared;
  if (!prepare_pdc_for(false, 0, nullptr, prepared)) return false;
  commit_pdc(prepared);
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
