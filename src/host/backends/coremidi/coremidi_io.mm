/// @file coremidi_io.mm
/// @brief CoreMIDI implementation of the MIDI I/O seams. See coremidi_io.h.

#include "host/backends/coremidi/coremidi_io.h"

#include <CoreMIDI/CoreMIDI.h>

#include <array>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host::backends {
namespace {

constexpr size_t kInputCapacity = 1024;
constexpr size_t kOutputCapacity = 1024;
constexpr size_t kDrainScratch = 256;

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

}  // namespace

// ===========================================================================
// CoreMidiInput
// ===========================================================================

struct CoreMidiInput::Impl {
  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef source = 0;
  FixedMidiInputSource<kInputCapacity> buffer;

  static void read_trampoline(const MIDIEventList* list, void* ref, void* /*srcRef*/) {
    static_cast<Impl*>(ref)->on_event_list(list);
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
        if (ump.word_count > 0) {
          // port_time_samples 0: the runtime stamps events to block start; a
          // host that needs sample-accurate input timing maps packet->timeStamp
          // through its own clock before pushing.
          buffer.push_event(ump, 0);
        }
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

void CoreMidiInput::close() noexcept {
  if (impl_->port != 0 && impl_->source != 0) MIDIPortDisconnectSource(impl_->port, impl_->source);
  if (impl_->port != 0) MIDIPortDispose(impl_->port);
  if (impl_->client != 0) MIDIClientDispose(impl_->client);
  impl_->port = 0;
  impl_->client = 0;
  impl_->source = 0;
}

bool CoreMidiInput::push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept {
  return impl_->buffer.push_event(ump, port_time_samples);
}

size_t CoreMidiInput::drain(midi::MidiEvent* out, size_t capacity,
                            int64_t block_start_frame) noexcept {
  return impl_->buffer.drain(out, capacity, block_start_frame);
}

size_t CoreMidiInput::pending_count() const noexcept { return impl_->buffer.pending_count(); }

// ===========================================================================
// CoreMidiOutput
// ===========================================================================

struct CoreMidiOutput::Impl {
  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef destination = 0;
  FixedMidiOutputSink<kOutputCapacity> queue;
  std::array<midi::MidiEvent, kDrainScratch> scratch{};
  const midi::SysExStore* sysex_store = nullptr;
};

CoreMidiOutput::CoreMidiOutput() : impl_(std::make_unique<Impl>()) {}
CoreMidiOutput::~CoreMidiOutput() { close(); }

size_t CoreMidiOutput::destination_count() { return MIDIGetNumberOfDestinations(); }

bool CoreMidiOutput::open(size_t destination_index) {
  if (impl_->client != 0) return false;
  if (destination_index >= MIDIGetNumberOfDestinations()) return false;
  impl_->destination = MIDIGetDestination(destination_index);
  if (impl_->destination == 0) return false;

  if (MIDIClientCreate(CFSTR("sonare.host.output"), nullptr, nullptr, &impl_->client) != noErr) {
    return false;
  }
  if (MIDIOutputPortCreate(impl_->client, CFSTR("sonare.out"), &impl_->port) != noErr) {
    close();
    return false;
  }
  return true;
}

void CoreMidiOutput::close() noexcept {
  if (impl_->port != 0) MIDIPortDispose(impl_->port);
  if (impl_->client != 0) MIDIClientDispose(impl_->client);
  impl_->port = 0;
  impl_->client = 0;
  impl_->destination = 0;
}

size_t CoreMidiOutput::flush_output() noexcept {
  if (impl_->port == 0 || impl_->destination == 0) return 0;
  const size_t n = impl_->queue.drain_queued(impl_->scratch.data(), impl_->scratch.size());
  if (n == 0) return 0;

  // Build one MIDIEventList from the drained UMP records. A SysEx-handle UMP is
  // resolved against the attached store (control thread) and expanded into
  // SysEx7 data packets; without a store or for an unknown handle it is skipped
  // (an unresolvable payload cannot be sent).
  std::array<uint8_t, sizeof(MIDIEventList) + kDrainScratch * sizeof(MIDIEventPacket)> storage{};
  auto* list = reinterpret_cast<MIDIEventList*>(storage.data());
  MIDIEventPacket* packet = MIDIEventListInit(list, kMIDIProtocol_2_0);
  const MIDITimeStamp now = 0;  // 0 = send immediately
  size_t sent = 0;
  for (size_t i = 0; i < n; ++i) {
    const midi::Ump& ump = impl_->scratch[i].ump;
    if (ump.word_count == 0) continue;
    if (ump.sysex_handle != 0) {
      const std::vector<uint8_t>* payload =
          impl_->sysex_store != nullptr ? impl_->sysex_store->lookup(ump.sysex_handle) : nullptr;
      if (payload == nullptr) continue;  // unresolvable: no store or unknown handle
      std::array<midi::Ump, kDrainScratch> packets{};
      const size_t count = midi::sysex7_payload_to_umps(payload->data(), payload->size(), ump.group,
                                                        packets.data(), packets.size());
      for (size_t k = 0; k < count; ++k) {
        packet = MIDIEventListAdd(list, storage.size(), packet, now, packets[k].word_count,
                                  packets[k].words);
      }
      if (count > 0) ++sent;
      continue;
    }
    packet = MIDIEventListAdd(list, storage.size(), packet, now, ump.word_count, ump.words);
    ++sent;
  }
  if (sent > 0) MIDISendEventList(impl_->port, impl_->destination, list);
  return sent;
}

void CoreMidiOutput::set_sysex_store(const midi::SysExStore* store) noexcept {
  impl_->sysex_store = store;
}

bool CoreMidiOutput::send(const midi::MidiEvent& event) noexcept {
  return impl_->queue.send(event);
}

size_t CoreMidiOutput::queued_count() const noexcept { return impl_->queue.queued_count(); }

}  // namespace sonare::host::backends
