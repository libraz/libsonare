/// @file coremidi_io.mm
/// @brief CoreMIDI implementation of the MIDI I/O seams. See coremidi_io.h.

#include "host/backends/coremidi/coremidi_io.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <vector>

#include "host/backends/coremidi/coremidi_output_state.h"
#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host::backends {
namespace {

constexpr size_t kInputCapacity = 1024;
constexpr size_t kOutputCapacity = 1024;
constexpr size_t kDrainScratch = 256;
// Per-call merge scratch for manually-injected events (see
// CoreMidiInput::Impl::drain_merged). Injection is UI-rate (an on-screen
// keyboard chord, a step-entry burst), never a firehose, so this is ample;
// smaller than kDrainScratch to keep the merge path's added stack cost low.
constexpr size_t kInjectedDrainScratch = 64;
constexpr size_t kMaxInputSysExBytes = 64 * 1024;
constexpr size_t kSysExStageDepth = 8;

/// Backing storage for a MIDIEventList sized for kDrainScratch packets
/// (~68 KB). A CoreMidiOutput::Impl member (constructed/zeroed once) rather
/// than a flush_output() local: MIDIEventListInit/MIDIEventListAdd populate
/// only the packets they append, so nothing reads the unwritten tail, and
/// flush_output() can run once per audio block — re-zeroing 68 KB on every
/// call would be a real per-call cost for no correctness benefit.
struct alignas(MIDIEventList) MidiEventListStorage {
  std::array<uint8_t, sizeof(MIDIEventList) + kDrainScratch * sizeof(MIDIEventPacket)> bytes{};
  MIDIEventList* list() noexcept { return reinterpret_cast<MIDIEventList*>(bytes.data()); }
  size_t size() const noexcept { return bytes.size(); }
};

/// Build a core Ump from a run of native-order UMP words starting at `words`.
/// Returns the number of words consumed (so a caller can walk a packet).
size_t ump_from_words(const uint32_t* words, size_t available, midi::Ump& out) noexcept {
  const uint8_t count = midi::ump_word_count_for_word0(words[0]);
  if (count > available) return available;  // truncated; consume the rest
  out = midi::Ump{};
  for (uint8_t i = 0; i < count; ++i) out.words[i] = words[i];
  out.word_count = count;
  out.group = static_cast<uint8_t>((words[0] >> 24) & 0x0Fu);
  return count;
}

/// Timestamp used for a UMP that carries no MIDIEventPacket of its own.
/// `absolute` marks a value already mapped to the engine's render-frame
/// timeline (what CoreMidiInput::push_event_at_host_time() produces);
/// otherwise it is the port-relative offset push_event() takes. File-scope
/// rather than nested in CoreMidiInput::Impl so it can be a defaulted
/// parameter of Impl's own member functions.
struct FallbackTime {
  int64_t value = 0;
  bool absolute = false;
};

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
  // kMaxInputSysExBytes up front so a producer thread grows it without ever
  // allocating on the realtime path.
  struct SysExAssembly {
    std::vector<uint8_t> bytes;
    bool active = false;
  };

  // A completed SysEx payload handed from a producer thread to the control
  // thread (the only thread that touches SysExStore). `bytes` is reserved to
  // kMaxInputSysExBytes so staging a payload copies without allocating.
  struct StagedSysEx {
    midi::SysExHandle handle = 0;
    std::vector<uint8_t> bytes;
  };

  // Reassembly + staging state for ONE SysEx7 producer thread. The
  // OS-registered read_trampoline (a live CoreMIDI source) and push_event()
  // (manual injection — an on-screen keyboard, step-entry pad, or similar,
  // driven by whatever thread the host calls it from) can run concurrently:
  // an on-screen keyboard alongside a connected hardware controller is an
  // ordinary configuration and must keep working, not stop the moment a
  // device is plugged in. Each producer gets its own SysExReassembler so
  // neither's group-keyed assembly state or staging ring is ever touched by
  // more than one thread — every operation below is "single writer" per
  // instance, not globally.
  struct SysExReassembler {
    std::array<SysExAssembly, 16> assemblies{};
    // Bounded single-producer/single-consumer staging ring: this
    // reassembler's owning thread is the sole producer, commit_staged_sysex()
    // (control thread) the sole consumer. Every slot reserves its payload
    // capacity at construction, so the producer only performs bounded copies
    // and never waits on the control thread. Release/acquire publication
    // keeps the payload write visible before the consumer reads the slot.
    std::array<StagedSysEx, kSysExStageDepth> stage{};
    std::atomic<size_t> stage_write{0};
    std::atomic<size_t> stage_read{0};

    void reserve() {
      for (auto& assembly : assemblies) assembly.bytes.reserve(kMaxInputSysExBytes);
      for (auto& staged : stage) staged.bytes.reserve(kMaxInputSysExBytes);
    }

    void reset() noexcept {
      for (auto& assembly : assemblies) {
        assembly.bytes.clear();  // keep reserved capacity for reuse
        assembly.active = false;
      }
      for (auto& staged : stage) {
        staged.handle = 0;
        staged.bytes.clear();  // keep reserved capacity for reuse
      }
      stage_write.store(0, std::memory_order_relaxed);
      stage_read.store(0, std::memory_order_relaxed);
    }
  };

  Impl() {
    live_sysex.reserve();
    injected_sysex.reserve();
  }

  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef source = 0;
  // Fed ONLY by the OS-registered read_trampoline while a live source is
  // connected — never by push_event()/push_event_at_host_time(), which target
  // `injected` below instead. Keeps this ring strictly single-producer
  // without gating manual injection on live-source state.
  FixedMidiInputSource<kInputCapacity> buffer;
  // Fed ONLY by push_event()/push_event_at_host_time(), on whatever thread
  // the host calls them from (typically one UI/control thread driving an
  // on-screen keyboard, step-entry pad, or similar — see host/midi_io.h's
  // push_event() contract). Always available, live source connected or not.
  // drain()/drain_block() merge this with `buffer` by render_frame.
  FixedMidiInputSource<kInputCapacity> injected;
  const MidiHostTimeMapper* time_mapper = nullptr;
  mach_timebase_info_data_t timebase{};
  // CONTROL thread only: never touched from a producer thread. Completed
  // payloads reach it through live_sysex.stage / injected_sysex.stage,
  // committed by commit_staged_sysex(). Handles from both reassemblers share
  // one namespace (next_input_handle below) so storing them in one SysExStore
  // is safe.
  midi::SysExStore sysex_store;
  SysExReassembler live_sysex;      // touched only by the OS callback thread
  SysExReassembler injected_sysex;  // touched only by push_event()'s caller
  // Handle allocator shared by both reassemblers. A plain atomic fetch_add is
  // safe for concurrent callers — unlike the SPSC rings above, this is not
  // single-producer.
  std::atomic<midi::SysExHandle> next_input_handle{1};
  std::atomic<uint32_t> sysex_overflow_count{0};
  std::atomic<uint32_t> sysex_interleave_count{0};

  static void read_trampoline(const MIDIEventList* list, void* ref, void* /*srcRef*/) {
    static_cast<Impl*>(ref)->on_event_list(list);
  }

  // `fallback` is used only when `packet` cannot be mapped to a render frame
  // (no packet, no host-time mapping, or no mapper attached): the live CoreMIDI
  // callback path always passes a real packet and leaves this at its default,
  // while push_event()/push_event_at_host_time() (which have no MIDIEventPacket
  // to derive a timestamp from) thread the caller's own timestamp through here
  // so an injected SysEx is not silently stamped at offset 0.
  bool enqueue_ump(FixedMidiInputSource<kInputCapacity>& target, const midi::Ump& ump,
                   const MIDIEventPacket* packet, FallbackTime fallback = {}) noexcept {
    uint64_t host_time_ns = 0;
    int64_t render_frame = 0;
    if (packet != nullptr && packet->timeStamp != 0 && time_mapper != nullptr &&
        host_ticks_to_ns(packet->timeStamp, timebase, &host_time_ns) &&
        time_mapper->host_time_to_render_frame(host_time_ns, &render_frame)) {
      return target.push_event_at_render_frame(ump, render_frame);
    }
    if (fallback.absolute) return target.push_event_at_render_frame(ump, fallback.value);
    return target.push_event(ump, fallback.value);
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

  // Producer thread: allocate a stable handle without touching the store.
  midi::SysExHandle allocate_input_handle() noexcept {
    midi::SysExHandle handle = next_input_handle.fetch_add(1, std::memory_order_relaxed);
    if (handle == 0) handle = next_input_handle.fetch_add(1, std::memory_order_relaxed);
    return handle;
  }

  // Producer thread (whichever owns `reassembler`): copy a completed payload
  // into its pre-reserved SPSC staging ring and return its handle, or 0 when
  // the ring is full.
  midi::SysExHandle stage_completed_sysex(SysExReassembler& reassembler,
                                          const std::vector<uint8_t>& payload) noexcept {
    const midi::SysExHandle handle = allocate_input_handle();
    const size_t write = reassembler.stage_write.load(std::memory_order_relaxed);
    const size_t read = reassembler.stage_read.load(std::memory_order_acquire);
    if (write - read >= kSysExStageDepth) {
      return 0;  // staging ring full; the caller drops the payload and counts it
    }
    StagedSysEx& slot = reassembler.stage[write % kSysExStageDepth];
    slot.handle = handle;
    slot.bytes.assign(payload.begin(), payload.end());  // within reserved capacity: no alloc
    reassembler.stage_write.store(write + 1, std::memory_order_release);
    return handle;
  }

  void emit_completed_sysex(FixedMidiInputSource<kInputCapacity>& target,
                            SysExReassembler& reassembler, uint8_t group, SysExAssembly* assembly,
                            const MIDIEventPacket* packet, FallbackTime fallback = {}) noexcept {
    assembly->bytes.push_back(0xF7u);
    const midi::SysExHandle handle = stage_completed_sysex(reassembler, assembly->bytes);
    assembly->bytes.clear();
    assembly->active = false;
    if (handle == 0) {
      sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    enqueue_ump(target, midi::make_sysex_handle(group, handle), packet, fallback);
  }

  // CONTROL thread: drain one reassembler's staged payloads into the store.
  // Each payload is copied from an SPSC-published slot then committed outside
  // the ring, so the store — and its allocation — stays entirely on this
  // thread and the producer never waits on a store operation.
  void commit_staged_sysex(SysExReassembler& reassembler) noexcept {
    // sysex_store() calls this unconditionally on every call for both
    // reassemblers, so bail out before paying the 64 KB scratch allocation
    // below when nothing is staged (the common case — most polls find an
    // empty ring).
    if (reassembler.stage_read.load(std::memory_order_relaxed) ==
        reassembler.stage_write.load(std::memory_order_acquire)) {
      return;
    }
    // Reserve drain scratch before the loop so copies remain allocation-free.
    std::vector<uint8_t> bytes;
    bytes.reserve(kMaxInputSysExBytes);
    for (;;) {
      const size_t read = reassembler.stage_read.load(std::memory_order_relaxed);
      const size_t write = reassembler.stage_write.load(std::memory_order_acquire);
      if (read == write) return;
      StagedSysEx& slot = reassembler.stage[read % kSysExStageDepth];
      const midi::SysExHandle handle = slot.handle;
      bytes.assign(slot.bytes.begin(), slot.bytes.end());  // within reserved capacity: no alloc
      reassembler.stage_read.store(read + 1, std::memory_order_release);
      bool stored = false;
      try {
        stored = sysex_store.add_with_handle(handle, bytes.data(), bytes.size());
      } catch (...) {
        stored = false;
      }
      if (!stored) sysex_overflow_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void commit_all_staged_sysex() noexcept {
    commit_staged_sysex(live_sysex);
    commit_staged_sysex(injected_sysex);
  }

  /// Returns true if this UMP was a SysEx7 Data64 packet and was consumed.
  /// `fallback` is forwarded to emit_completed_sysex/enqueue_ump for the
  /// injection callers (see enqueue_ump); the live callback path
  /// (packet != nullptr) ignores it in favor of the packet's own timestamp.
  bool consume_sysex7(FixedMidiInputSource<kInputCapacity>& target, SysExReassembler& reassembler,
                      const midi::Ump& ump, const MIDIEventPacket* packet,
                      FallbackTime fallback = {}) noexcept {
    if (((ump.words[0] >> 28u) & 0x0Fu) != 0x3u) return false;
    // ump.group is already masked to 0x0F when built from wire bytes (see
    // ump_from_words), but this is also reachable from the public push_event()
    // API with a caller-constructed Ump, which carries no such guarantee. Mask
    // here so the assemblies[] index below never leaves its 16 slots.
    const uint8_t group = static_cast<uint8_t>(ump.group & 0x0Fu);
    const uint8_t status = static_cast<uint8_t>((ump.words[0] >> 20u) & 0x0Fu);
    SysExAssembly& assembly = reassembler.assemblies[group];
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
          if (append_sysex_bytes(&assembly, ump)) {
            emit_completed_sysex(target, reassembler, group, &assembly, packet, fallback);
          }
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
          if (append_sysex_bytes(&assembly, ump)) {
            emit_completed_sysex(target, reassembler, group, &assembly, packet, fallback);
          }
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
        if (ump.word_count > 0 && !consume_sysex7(buffer, live_sysex, ump, packet)) {
          enqueue_ump(buffer, ump, packet);
        }
        i += consumed;
      }
      packet = MIDIEventPacketNext(packet);
    }
  }

  // AUDIO thread: merge `buffer` (live source) with `injected` (manual
  // injection) into `out`, ordered by render_frame. Only called when
  // `injected` actually has something pending (see CoreMidiInput::drain()) —
  // the common case of a live-only or injection-only block takes a cheaper
  // direct path there and never reaches this. Bounded per-call scratch: no
  // allocation, matching drain()'s RT-safety contract.
  size_t drain_merged(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                      int num_frames) noexcept {
    if (out == nullptr || capacity == 0) return 0;
    std::array<midi::MidiEvent, kDrainScratch> live_scratch{};
    std::array<midi::MidiEvent, kInjectedDrainScratch> injected_scratch{};
    const size_t live_cap = std::min(capacity, kDrainScratch);
    const size_t injected_cap = std::min(capacity, kInjectedDrainScratch);
    const size_t n1 =
        num_frames >= 0
            ? buffer.drain_block(live_scratch.data(), live_cap, block_start_frame, num_frames)
            : buffer.drain(live_scratch.data(), live_cap, block_start_frame);
    const size_t n2 =
        num_frames >= 0 ? injected.drain_block(injected_scratch.data(), injected_cap,
                                               block_start_frame, num_frames)
                        : injected.drain(injected_scratch.data(), injected_cap, block_start_frame);
    // Both halves are individually sorted by render_frame already (each
    // FixedMidiInputSource::drain[_block] sorts its own output); a stable
    // two-pointer merge is enough, no re-sort needed.
    size_t i = 0, j = 0, n = 0;
    while (i < n1 && j < n2 && n < capacity) {
      if (live_scratch[i].render_frame <= injected_scratch[j].render_frame) {
        out[n++] = live_scratch[i++];
      } else {
        out[n++] = injected_scratch[j++];
      }
    }
    while (i < n1 && n < capacity) out[n++] = live_scratch[i++];
    while (j < n2 && n < capacity) out[n++] = injected_scratch[j++];
    return n;
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
  // From here the OS-registered read_trampoline is the sole producer into
  // `buffer` and `live_sysex`. push_event()/push_event_at_host_time() keep
  // working while the source is live because they produce into `injected` and
  // `injected_sysex` instead, so each ring still has exactly one writer.
  return true;
}

void CoreMidiInput::set_time_mapper(const MidiHostTimeMapper* mapper) noexcept {
  impl_->time_mapper = mapper;
}

const midi::SysExStore* CoreMidiInput::sysex_store() const noexcept {
  // Commit any payloads staged by either producer (the MIDI callback and the
  // injection caller) before exposing the store, so a control-thread lookup
  // resolves reassembled SysEx from both. The store is only ever mutated here,
  // on the control thread.
  impl_->commit_all_staged_sysex();
  return &impl_->sysex_store;
}

bool CoreMidiInput::push_event_at_host_time(const midi::Ump& ump, uint64_t host_time_ns) noexcept {
  // Produces into `injected`, never into `buffer`, so this stays legal while a
  // live CoreMIDI source is connected: each ring keeps exactly one writer (see
  // the `injected` member and push_event()).
  FallbackTime fallback;
  int64_t render_frame = 0;
  if (impl_->time_mapper != nullptr &&
      impl_->time_mapper->host_time_to_render_frame(host_time_ns, &render_frame)) {
    fallback = {render_frame, true};
  }
  // Reassemble here for the same reason push_event() does: a multi-packet
  // SysEx7 injected through this entry point must reach the store as one
  // payload rather than surfacing as raw Data64 UMPs the consumer cannot
  // resolve. The mapped render frame rides along so the completed SysEx lands
  // on the same timeline as the packets that carried it.
  if (impl_->consume_sysex7(impl_->injected, impl_->injected_sysex, ump, nullptr, fallback)) {
    return true;
  }
  if (fallback.absolute) return impl_->injected.push_event_at_render_frame(ump, fallback.value);
  return impl_->injected.push_event(ump, 0);
}

void CoreMidiInput::close() noexcept {
  if (impl_->port != 0 && impl_->source != 0) MIDIPortDisconnectSource(impl_->port, impl_->source);
  if (impl_->port != 0) MIDIPortDispose(impl_->port);
  if (impl_->client != 0) MIDIClientDispose(impl_->client);
  impl_->port = 0;
  impl_->client = 0;
  impl_->source = 0;
  // Disposing the port/client above is synchronous, so read_trampoline cannot
  // fire again once it returns and this control-thread reset races nothing on
  // the live side. `injected` and `injected_sysex` are reset too: close() ends
  // the timeline both rings were stamped against, so anything still queued
  // there would surface against the next device's frame numbering.
  impl_->buffer.clear();
  impl_->injected.clear();
  impl_->live_sysex.reset();
  impl_->injected_sysex.reset();
  impl_->sysex_store.clear();
}

bool CoreMidiInput::push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept {
  // Manual injection (an on-screen keyboard, a step-entry pad) produces into
  // `injected` and `injected_sysex`, which no other thread writes. The live
  // CoreMIDI callback owns `buffer` and `live_sysex`. Two producers, two rings,
  // so host/midi_io.h's single-producer invariant is satisfied by construction
  // and injection keeps working while a device is connected.
  //
  // consume_sysex7 has no MIDIEventPacket to derive a timestamp from here, so
  // the caller's port_time_samples is threaded through as the fallback used
  // when a completed SysEx is finally enqueued (see enqueue_ump) — otherwise
  // it would be silently discarded in favor of a hardcoded 0.
  if (impl_->consume_sysex7(impl_->injected, impl_->injected_sysex, ump, nullptr,
                            {port_time_samples, false})) {
    return true;
  }
  return impl_->injected.push_event(ump, port_time_samples);
}

size_t CoreMidiInput::drain(midi::MidiEvent* out, size_t capacity,
                            int64_t block_start_frame) noexcept {
  // Skip the merge whenever one side is empty, which is the usual case: a
  // hardware-only session never injects, and an injection-only session has no
  // live source. Only a genuinely mixed block pays for the scratch buffers.
  if (impl_->injected.pending_count() == 0) {
    return impl_->buffer.drain(out, capacity, block_start_frame);
  }
  if (impl_->buffer.pending_count() == 0) {
    return impl_->injected.drain(out, capacity, block_start_frame);
  }
  return impl_->drain_merged(out, capacity, block_start_frame, -1);
}

size_t CoreMidiInput::drain_block(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                                  int num_frames) noexcept {
  if (impl_->injected.pending_count() == 0) {
    return impl_->buffer.drain_block(out, capacity, block_start_frame, num_frames);
  }
  if (impl_->buffer.pending_count() == 0) {
    return impl_->injected.drain_block(out, capacity, block_start_frame, num_frames);
  }
  return impl_->drain_merged(out, capacity, block_start_frame, num_frames);
}

size_t CoreMidiInput::pending_count() const noexcept {
  return impl_->buffer.pending_count() + impl_->injected.pending_count();
}

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
  // Reused across flush_output() calls; see MidiEventListStorage's comment.
  MidiEventListStorage event_list_storage{};
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

  // Reused across calls (see the MidiEventListStorage / event_list_storage
  // comments) rather than a fresh ~68 KB zero-initialized local every time.
  MidiEventListStorage& storage = impl_->event_list_storage;
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
    const size_t available = impl_->pending_count.load(std::memory_order_relaxed) - completed;
    const auto batch = detail::flush_fixed_batch(
        available,
        [&](size_t index) noexcept {
          const auto& candidate = impl_->pending[completed + index].event;
          if (!detail::fixed_batch_event_is_sendable(candidate.ump)) return false;
          MIDIEventPacket* next =
              MIDIEventListAdd(list, storage.size(), packet, timestamp_for(candidate),
                               candidate.ump.word_count, candidate.ump.words);
          if (next == nullptr) return false;
          packet = next;
          return true;
        },
        [&]() noexcept {
          return MIDISendEventList(impl_->port, impl_->destination, list) == noErr;
        });
    if (batch.status == detail::BatchFlushStatus::kRejected) {
      // The head event cannot be packed on an empty list: either its word_count
      // contradicts its message type (rejected here, before the SDK sees it) or
      // CoreMIDI refused it outright. Either way no list will ever accept it, so
      // count it and drop it, like an invalid SysEx, and let the events queued
      // behind it still reach the device.
      ++impl_->send_error_count;
      ++completed;
      continue;
    }
    if (batch.status == detail::BatchFlushStatus::kRetry) {
      ++impl_->send_error_count;
      retain_from(completed);
      return sent;
    }
    completed += batch.batch_count;
    sent += batch.batch_count;
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
