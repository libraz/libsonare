/// @file coremidi_io.mm
/// @brief CoreMIDI implementation of the MIDI I/O seams. See coremidi_io.h.

#include "host/backends/coremidi/coremidi_io.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

#include "host/backends/coremidi/coremidi_output_state.h"
#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host::backends {
namespace {

constexpr size_t kInputCapacity = 1024;
constexpr size_t kOutputCapacity = 1024;
constexpr size_t kDrainScratch = 256;
constexpr size_t kMaxInputSysExBytes = 64 * 1024;
constexpr size_t kSysExStageDepth = 8;

/// Active 32-bit word count for a UMP message-type nibble (UMP spec §2.1.4).
uint8_t words_for_message_type(uint8_t mt) noexcept {
  switch (mt) {
    case 0x0:  // Utility
    case 0x1:  // System real time / common
    case 0x2:  // MIDI 1.0 channel voice
    case 0x6:  // reserved (32-bit)
    case 0x7:  // reserved (32-bit)
      return 1;
    case 0x3:  // Data (SysEx7, 64-bit)
    case 0x4:  // MIDI 2.0 channel voice
    case 0x8:  // reserved (64-bit)
    case 0x9:
    case 0xA:
      return 2;
    case 0xB:  // reserved (96-bit)
    case 0xC:
      return 3;
    default:  // 0x5 (128-bit data), 0xD-0xF
      return 4;
  }
}

/// Build a core Ump from a run of native-order UMP words starting at `words`.
/// Returns the number of words consumed (so a caller can walk a packet).
size_t ump_from_words(const uint32_t* words, size_t available, midi::Ump& out) noexcept {
  const uint8_t mt = static_cast<uint8_t>((words[0] >> 28) & 0x0Fu);
  const uint8_t count = words_for_message_type(mt);
  if (count > available) return available;  // truncated; consume the rest
  out = midi::Ump{};
  for (uint8_t i = 0; i < count; ++i) out.words[i] = words[i];
  out.word_count = count;
  out.group = static_cast<uint8_t>((words[0] >> 24) & 0x0Fu);
  return count;
}

bool host_ticks_to_ns(uint64_t ticks, const mach_timebase_info_data_t& timebase,
                      uint64_t* out) noexcept {
  if (out == nullptr || timebase.denom == 0) return false;
  const unsigned __int128 scaled = static_cast<unsigned __int128>(ticks) *
                                   static_cast<unsigned __int128>(timebase.numer) /
                                   static_cast<unsigned __int128>(timebase.denom);
  if (scaled > std::numeric_limits<uint64_t>::max()) return false;
  *out = static_cast<uint64_t>(scaled);
  return true;
}

bool ns_to_host_ticks(uint64_t nanoseconds, const mach_timebase_info_data_t& timebase,
                      MIDITimeStamp* out) noexcept {
  if (out == nullptr || timebase.numer == 0) return false;
  const unsigned __int128 scaled = static_cast<unsigned __int128>(nanoseconds) *
                                   static_cast<unsigned __int128>(timebase.denom) /
                                   static_cast<unsigned __int128>(timebase.numer);
  if (scaled > std::numeric_limits<MIDITimeStamp>::max()) return false;
  *out = static_cast<MIDITimeStamp>(scaled);
  return true;
}

}  // namespace

// ===========================================================================
// CoreMidiInput
// ===========================================================================

struct CoreMidiInput::Impl {
  // Per-group reassembly buffer for multi-packet SysEx7. `bytes` is reserved to
  // kMaxInputSysExBytes up front so the MIDI callback grows it without ever
  // allocating on the realtime path.
  struct SysExAssembly {
    std::vector<uint8_t> bytes;
    bool active = false;
  };

  // A completed SysEx payload handed from the MIDI callback (producer) to the
  // control thread (consumer), which is the only thread that touches the
  // SysExStore. `bytes` is reserved to kMaxInputSysExBytes so staging a payload
  // copies without allocating on the callback.
  struct StagedSysEx {
    midi::SysExHandle handle = 0;
    std::vector<uint8_t> bytes;
  };

  Impl() {
    for (auto& assembly : sysex_assemblies) assembly.bytes.reserve(kMaxInputSysExBytes);
    for (auto& staged : sysex_stage) staged.bytes.reserve(kMaxInputSysExBytes);
  }

  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef source = 0;
  FixedMidiInputSource<kInputCapacity> buffer;
  const MidiHostTimeMapper* time_mapper = nullptr;
  mach_timebase_info_data_t timebase{};
  // CONTROL thread only: never touched from the MIDI callback. Completed payloads
  // reach it through sysex_stage, committed by commit_staged_sysex().
  midi::SysExStore sysex_store;
  std::array<SysExAssembly, 16> sysex_assemblies{};
  // Bounded producer/consumer staging ring between the MIDI callback and the
  // control thread, guarded by sysex_mutex. Each critical section is a single
  // bounded byte copy; neither thread holds the lock across an allocation or a
  // blocking call.
  std::array<StagedSysEx, kSysExStageDepth> sysex_stage{};
  std::mutex sysex_mutex;
  size_t sysex_stage_write = 0;
  size_t sysex_stage_read = 0;
  // Callback-side handle allocator so a staged payload carries a stable handle
  // before the control thread commits its bytes to the store.
  std::atomic<midi::SysExHandle> next_input_handle{1};
  std::atomic<uint32_t> sysex_overflow_count{0};
  std::atomic<uint32_t> sysex_interleave_count{0};

  static void read_trampoline(const MIDIEventList* list, void* ref, void* /*srcRef*/) {
    static_cast<Impl*>(ref)->on_event_list(list);
  }

  bool enqueue_ump(const midi::Ump& ump, const MIDIEventPacket* packet) noexcept {
    uint64_t host_time_ns = 0;
    int64_t render_frame = 0;
    if (packet != nullptr && packet->timeStamp != 0 && time_mapper != nullptr &&
        host_ticks_to_ns(packet->timeStamp, timebase, &host_time_ns) &&
        time_mapper->host_time_to_render_frame(host_time_ns, &render_frame)) {
      return buffer.push_event_at_render_frame(ump, render_frame);
    }
    return buffer.push_event(ump, 0);
  }

  bool append_sysex_bytes(SysExAssembly* assembly, const midi::Ump& ump) noexcept {
    const uint8_t count = static_cast<uint8_t>((ump.words[0] >> 16u) & 0x0Fu);
    if (count > 6 || assembly->bytes.size() + count + 1 > kMaxInputSysExBytes) {
      sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
      assembly->bytes.clear();
      assembly->active = false;
      return false;
    }
    const uint8_t bytes[6] = {
        static_cast<uint8_t>((ump.words[0] >> 8u) & 0xFFu),
        static_cast<uint8_t>(ump.words[0] & 0xFFu),
        static_cast<uint8_t>((ump.words[1] >> 24u) & 0xFFu),
        static_cast<uint8_t>((ump.words[1] >> 16u) & 0xFFu),
        static_cast<uint8_t>((ump.words[1] >> 8u) & 0xFFu),
        static_cast<uint8_t>(ump.words[1] & 0xFFu),
    };
    for (uint8_t i = 0; i < count; ++i) {
      if (bytes[i] > 0x7Fu) {
        sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
        assembly->bytes.clear();
        assembly->active = false;
        return false;
      }
      assembly->bytes.push_back(bytes[i]);
    }
    return true;
  }

  // MIDI callback thread: allocate a stable handle without touching the store.
  midi::SysExHandle allocate_input_handle() noexcept {
    midi::SysExHandle handle = next_input_handle.fetch_add(1, std::memory_order_relaxed);
    if (handle == 0) handle = next_input_handle.fetch_add(1, std::memory_order_relaxed);
    return handle;
  }

  // MIDI callback thread: copy a completed payload into the staging ring under a
  // brief lock (a bounded memcpy into a pre-reserved slot, no allocation) and
  // return its handle, or 0 when the ring is full.
  midi::SysExHandle stage_completed_sysex(const std::vector<uint8_t>& payload) noexcept {
    const midi::SysExHandle handle = allocate_input_handle();
    std::lock_guard<std::mutex> guard(sysex_mutex);
    if (sysex_stage_write - sysex_stage_read >= kSysExStageDepth) {
      return 0;  // staging ring full; the caller drops the payload and counts it
    }
    StagedSysEx& slot = sysex_stage[sysex_stage_write % kSysExStageDepth];
    slot.handle = handle;
    slot.bytes.assign(payload.begin(), payload.end());  // within reserved capacity: no alloc
    ++sysex_stage_write;
    return handle;
  }

  void emit_completed_sysex(uint8_t group, SysExAssembly* assembly,
                            const MIDIEventPacket* packet) noexcept {
    assembly->bytes.push_back(0xF7u);
    const midi::SysExHandle handle = stage_completed_sysex(assembly->bytes);
    assembly->bytes.clear();
    assembly->active = false;
    if (handle == 0) {
      sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    enqueue_ump(midi::make_sysex_handle(group, handle), packet);
  }

  // CONTROL thread: drain staged payloads into the store. Each payload is copied
  // out under the lock (a bounded memcpy) and committed to the store outside the
  // lock, so the store — and its allocation — stays entirely on this thread and
  // the callback never waits on a store operation.
  void commit_staged_sysex() noexcept {
    for (;;) {
      midi::SysExHandle handle = 0;
      std::vector<uint8_t> bytes;
      {
        std::lock_guard<std::mutex> guard(sysex_mutex);
        if (sysex_stage_read == sysex_stage_write) return;
        StagedSysEx& slot = sysex_stage[sysex_stage_read % kSysExStageDepth];
        handle = slot.handle;
        bytes.assign(slot.bytes.begin(), slot.bytes.end());
        ++sysex_stage_read;
      }
      bool stored = false;
      try {
        stored = sysex_store.add_with_handle(handle, bytes.data(), bytes.size());
      } catch (...) {
        stored = false;
      }
      if (!stored) sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  /// Returns true if this UMP was a SysEx7 Data64 packet and was consumed.
  bool consume_sysex7(const midi::Ump& ump, const MIDIEventPacket* packet) noexcept {
    if (((ump.words[0] >> 28u) & 0x0Fu) != 0x3u) return false;
    const uint8_t group = ump.group;
    const uint8_t status = static_cast<uint8_t>((ump.words[0] >> 20u) & 0x0Fu);
    SysExAssembly& assembly = sysex_assemblies[group];
    try {
      switch (status) {
        case 0:  // Complete.
          if (assembly.active) {
            sysex_interleave_count.fetch_add(1, std::memory_order_relaxed);
            assembly.bytes.clear();
            assembly.active = false;
          }
          assembly.bytes = {0xF0u};
          assembly.active = true;
          if (append_sysex_bytes(&assembly, ump)) emit_completed_sysex(group, &assembly, packet);
          return true;
        case 1:  // Start.
          if (assembly.active) sysex_interleave_count.fetch_add(1, std::memory_order_relaxed);
          assembly.bytes = {0xF0u};
          assembly.active = true;
          append_sysex_bytes(&assembly, ump);
          return true;
        case 2:  // Continue.
          if (!assembly.active) {
            sysex_interleave_count.fetch_add(1, std::memory_order_relaxed);
            return true;
          }
          append_sysex_bytes(&assembly, ump);
          return true;
        case 3:  // End.
          if (!assembly.active) {
            sysex_interleave_count.fetch_add(1, std::memory_order_relaxed);
            return true;
          }
          if (append_sysex_bytes(&assembly, ump)) emit_completed_sysex(group, &assembly, packet);
          return true;
        default:
          sysex_interleave_count.fetch_add(1, std::memory_order_relaxed);
          return true;
      }
    } catch (...) {
      sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
      assembly.bytes.clear();
      assembly.active = false;
      return true;
    }
  }

  void on_event_list(const MIDIEventList* list) noexcept {
    if (list == nullptr) return;
    const MIDIEventPacket* packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
      size_t i = 0;
      const size_t n = packet->wordCount;
      while (i < n) {
        midi::Ump ump;
        const size_t consumed = ump_from_words(&packet->words[i], n - i, ump);
        if (consumed == 0) break;
        if (ump.word_count > 0 && !consume_sysex7(ump, packet)) enqueue_ump(ump, packet);
        i += consumed;
      }
      packet = MIDIEventPacketNext(packet);
    }
  }
};

CoreMidiInput::CoreMidiInput() : impl_(std::make_unique<Impl>()) {}
CoreMidiInput::~CoreMidiInput() { close(); }

size_t CoreMidiInput::source_count() { return MIDIGetNumberOfSources(); }

bool CoreMidiInput::open(size_t source_index) {
  if (impl_->client != 0) return false;
  if (source_index >= MIDIGetNumberOfSources()) return false;
  impl_->source = MIDIGetSource(source_index);
  if (impl_->source == 0) return false;
  mach_timebase_info(&impl_->timebase);

  if (MIDIClientCreate(CFSTR("sonare.host.input"), nullptr, nullptr, &impl_->client) != noErr) {
    return false;
  }
  if (MIDIInputPortCreateWithProtocol(impl_->client, CFSTR("sonare.in"), kMIDIProtocol_2_0,
                                      &impl_->port, ^(const MIDIEventList* list, void* srcRef) {
                                        Impl::read_trampoline(list, impl_.get(), srcRef);
                                      }) != noErr) {
    close();
    return false;
  }
  if (MIDIPortConnectSource(impl_->port, impl_->source, nullptr) != noErr) {
    close();
    return false;
  }
  return true;
}

void CoreMidiInput::set_time_mapper(const MidiHostTimeMapper* mapper) noexcept {
  impl_->time_mapper = mapper;
}

const midi::SysExStore* CoreMidiInput::sysex_store() const noexcept {
  // Commit any payloads staged by the MIDI callback before exposing the store,
  // so a control-thread lookup resolves reassembled SysEx. The store is only ever
  // mutated here, on the control thread.
  impl_->commit_staged_sysex();
  return &impl_->sysex_store;
}

bool CoreMidiInput::push_event_at_host_time(const midi::Ump& ump, uint64_t host_time_ns) noexcept {
  int64_t render_frame = 0;
  if (impl_->time_mapper != nullptr &&
      impl_->time_mapper->host_time_to_render_frame(host_time_ns, &render_frame)) {
    return impl_->buffer.push_event_at_render_frame(ump, render_frame);
  }
  return impl_->buffer.push_event(ump, 0);
}

void CoreMidiInput::close() noexcept {
  if (impl_->port != 0 && impl_->source != 0) MIDIPortDisconnectSource(impl_->port, impl_->source);
  if (impl_->port != 0) MIDIPortDispose(impl_->port);
  if (impl_->client != 0) MIDIClientDispose(impl_->client);
  impl_->port = 0;
  impl_->client = 0;
  impl_->source = 0;
  impl_->buffer.clear();
  {
    std::lock_guard<std::mutex> guard(impl_->sysex_mutex);
    impl_->sysex_stage_read = 0;
    impl_->sysex_stage_write = 0;
    for (auto& staged : impl_->sysex_stage) {
      staged.handle = 0;
      staged.bytes.clear();  // keep reserved capacity for reuse
    }
  }
  impl_->sysex_store.clear();
  for (auto& assembly : impl_->sysex_assemblies) {
    assembly.bytes.clear();  // keep reserved capacity for reuse
    assembly.active = false;
  }
}

bool CoreMidiInput::push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept {
  if (impl_->consume_sysex7(ump, nullptr)) return true;
  return impl_->buffer.push_event(ump, port_time_samples);
}

size_t CoreMidiInput::drain(midi::MidiEvent* out, size_t capacity,
                            int64_t block_start_frame) noexcept {
  return impl_->buffer.drain(out, capacity, block_start_frame);
}

size_t CoreMidiInput::drain_block(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                                  int num_frames) noexcept {
  return impl_->buffer.drain_block(out, capacity, block_start_frame, num_frames);
}

size_t CoreMidiInput::pending_count() const noexcept { return impl_->buffer.pending_count(); }

uint32_t CoreMidiInput::sysex_overflow_count() const noexcept {
  return impl_->sysex_overflow_count.load(std::memory_order_relaxed);
}

uint32_t CoreMidiInput::sysex_interleave_count() const noexcept {
  return impl_->sysex_interleave_count.load(std::memory_order_relaxed);
}

// ===========================================================================
// CoreMidiOutput
// ===========================================================================

struct CoreMidiOutput::Impl {
  struct PendingEvent {
    midi::MidiEvent event{};
    size_t sysex_packet_position = 0;
  };

  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef destination = 0;
  FixedMidiOutputSink<kOutputCapacity> queue;
  std::array<midi::MidiEvent, kDrainScratch> scratch{};
  std::array<PendingEvent, kDrainScratch> pending{};
  std::array<midi::Ump, kDrainScratch> sysex_packets{};
  std::atomic<size_t> pending_count{0};
  const midi::SysExStore* sysex_store = nullptr;
  const MidiHostTimeMapper* time_mapper = nullptr;
  mach_timebase_info_data_t timebase{};
  uint32_t send_error_count = 0;
  uint32_t invalid_sysex_count = 0;
};

CoreMidiOutput::CoreMidiOutput() : impl_(std::make_unique<Impl>()) {}
CoreMidiOutput::~CoreMidiOutput() { close(); }

size_t CoreMidiOutput::destination_count() { return MIDIGetNumberOfDestinations(); }

bool CoreMidiOutput::open(size_t destination_index) {
  if (impl_->client != 0) return false;
  if (destination_index >= MIDIGetNumberOfDestinations()) return false;
  impl_->destination = MIDIGetDestination(destination_index);
  if (impl_->destination == 0) return false;
  mach_timebase_info(&impl_->timebase);

  if (MIDIClientCreate(CFSTR("sonare.host.output"), nullptr, nullptr, &impl_->client) != noErr) {
    return false;
  }
  if (MIDIOutputPortCreate(impl_->client, CFSTR("sonare.out"), &impl_->port) != noErr) {
    close();
    return false;
  }
  return true;
}

void CoreMidiOutput::set_time_mapper(const MidiHostTimeMapper* mapper) noexcept {
  impl_->time_mapper = mapper;
}

void CoreMidiOutput::close() noexcept {
  if (impl_->port != 0) MIDIPortDispose(impl_->port);
  if (impl_->client != 0) MIDIClientDispose(impl_->client);
  impl_->port = 0;
  impl_->client = 0;
  impl_->destination = 0;
  impl_->queue.clear();
  impl_->pending_count.store(0, std::memory_order_release);
  for (auto& pending : impl_->pending) pending = {};
}

size_t CoreMidiOutput::flush_output() noexcept {
  if (impl_->port == 0 || impl_->destination == 0) return 0;
  if (impl_->pending_count.load(std::memory_order_acquire) == 0) {
    const size_t n = impl_->queue.drain_queued(impl_->scratch.data(), impl_->scratch.size());
    if (n == 0) return 0;
    std::stable_sort(impl_->scratch.begin(), impl_->scratch.begin() + n,
                     [](const midi::MidiEvent& a, const midi::MidiEvent& b) {
                       return a.render_frame < b.render_frame;
                     });
    // Reset the SysEx packet cursor for every slot: a slot reused across flush
    // cycles must start a fresh payload from packet 0, otherwise a stale cursor
    // from a previous SysEx drops, truncates, or zero-sends the new message.
    for (size_t i = 0; i < n; ++i) impl_->pending[i] = Impl::PendingEvent{impl_->scratch[i], 0};
    impl_->pending_count.store(n, std::memory_order_release);
  }

  const auto timestamp_for = [&](const midi::MidiEvent& event) noexcept {
    MIDITimeStamp timestamp = 0;  // 0 = immediate when no audio-clock anchor exists
    uint64_t host_time_ns = 0;
    if (impl_->time_mapper != nullptr &&
        impl_->time_mapper->render_frame_to_host_time(event.render_frame, &host_time_ns)) {
      ns_to_host_ticks(host_time_ns, impl_->timebase, &timestamp);
    }
    return timestamp;
  };

  const auto retain_from = [&](size_t completed) noexcept {
    if (completed == 0) return;
    const size_t total = impl_->pending_count.load(std::memory_order_relaxed);
    const size_t remaining = total - completed;
    for (size_t i = 0; i < remaining; ++i) impl_->pending[i] = impl_->pending[completed + i];
    impl_->pending_count.store(remaining, std::memory_order_release);
  };

  struct alignas(MIDIEventList) MidiEventListStorage {
    std::array<uint8_t, sizeof(MIDIEventList) + kDrainScratch * sizeof(MIDIEventPacket)> bytes{};
    MIDIEventList* list() noexcept { return reinterpret_cast<MIDIEventList*>(bytes.data()); }
    size_t size() const noexcept { return bytes.size(); }
  } storage;
  size_t sent = 0;
  size_t completed = 0;
  while (completed < impl_->pending_count.load(std::memory_order_relaxed)) {
    auto& pending = impl_->pending[completed];
    const midi::Ump& ump = pending.event.ump;
    if (ump.word_count == 0) {
      ++completed;
      continue;
    }

    if (ump.sysex_handle != 0) {
      const std::vector<uint8_t>* payload =
          impl_->sysex_store != nullptr ? impl_->sysex_store->lookup(ump.sysex_handle) : nullptr;
      const auto status = detail::flush_sysex7_payload(
          payload != nullptr ? payload->data() : nullptr, payload != nullptr ? payload->size() : 0,
          ump.group, &pending.sysex_packet_position, impl_->sysex_packets.data(),
          impl_->sysex_packets.size(), [&](const midi::Ump* packets, size_t count) noexcept {
            auto* list = storage.list();
            MIDIEventPacket* packet = MIDIEventListInit(list, kMIDIProtocol_2_0);
            const MIDITimeStamp timestamp = timestamp_for(pending.event);
            for (size_t i = 0; i < count; ++i) {
              packet = MIDIEventListAdd(list, storage.size(), packet, timestamp,
                                        packets[i].word_count, packets[i].words);
              if (packet == nullptr) return false;
            }
            return MIDISendEventList(impl_->port, impl_->destination, list) == noErr;
          });
      if (status == detail::SysExFlushStatus::kInvalid) {
        ++impl_->invalid_sysex_count;
        ++completed;
        continue;
      }
      if (status == detail::SysExFlushStatus::kRetry) {
        ++impl_->send_error_count;
        retain_from(completed);
        return sent;
      }
      ++completed;
      ++sent;
      continue;
    }

    // Batch adjacent fixed-size events into one EventList. Advance/remove them
    // only after CoreMIDI accepts the whole list, so a failure is retryable.
    auto* list = storage.list();
    MIDIEventPacket* packet = MIDIEventListInit(list, kMIDIProtocol_2_0);
    size_t batch_count = 0;
    while (completed + batch_count < impl_->pending_count.load(std::memory_order_relaxed)) {
      const auto& candidate = impl_->pending[completed + batch_count].event;
      if (candidate.ump.word_count == 0 || candidate.ump.sysex_handle != 0) break;
      MIDIEventPacket* next =
          MIDIEventListAdd(list, storage.size(), packet, timestamp_for(candidate),
                           candidate.ump.word_count, candidate.ump.words);
      if (next == nullptr) break;
      packet = next;
      ++batch_count;
    }
    if (batch_count == 0 || MIDISendEventList(impl_->port, impl_->destination, list) != noErr) {
      ++impl_->send_error_count;
      retain_from(completed);
      return sent;
    }
    completed += batch_count;
    sent += batch_count;
  }
  impl_->pending_count.store(0, std::memory_order_release);
  return sent;
}

void CoreMidiOutput::set_sysex_store(const midi::SysExStore* store) noexcept {
  impl_->sysex_store = store;
}

bool CoreMidiOutput::send(const midi::MidiEvent& event) noexcept {
  return impl_->queue.send(event);
}

size_t CoreMidiOutput::queued_count() const noexcept {
  return impl_->pending_count.load(std::memory_order_acquire) + impl_->queue.queued_count();
}

uint32_t CoreMidiOutput::send_error_count() const noexcept { return impl_->send_error_count; }

uint32_t CoreMidiOutput::invalid_sysex_count() const noexcept { return impl_->invalid_sysex_count; }

}  // namespace sonare::host::backends
