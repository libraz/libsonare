#pragma once

/// @file coremidi_io.h
/// @brief CoreMIDI implementations of the sonare::host::MidiInputSource /
///        MidiOutputSink seams. macOS only; built behind BUILD_COREMIDI.
///
/// The seam is UMP-native: the input port is created with kMIDIProtocol_2_0 so
/// MIDI 2.0 / MPE pass through losslessly. This public header includes NO
/// CoreMIDI headers — the MIDIClientRef / MIDIPortRef live behind a pimpl in
/// the .mm (invariant 6: the seam stays SDK-free).
///
/// Threading mirrors the seam contract: CoreMIDI delivers incoming packets on
/// its own callback thread (push into the input source there); the RT runtime
/// drains them at block start (drain_block); the runtime sends output events
/// (send, RT-safe) and a control/port thread flushes them to the device via
/// flush_output().

#include <cstddef>
#include <cstdint>
#include <memory>

#include "host/midi_io.h"

namespace sonare::midi {
class SysExStore;
}  // namespace sonare::midi

namespace sonare::host::backends {

/// Live CoreMIDI input: connects to a source endpoint and pushes incoming UMP
/// into an internal fixed buffer the RT runtime drains. IS-A MidiInputSource so
/// it plugs straight into the engine's MIDI input wiring.
class CoreMidiInput final : public MidiInputSource {
 public:
  CoreMidiInput();
  ~CoreMidiInput() override;

  CoreMidiInput(const CoreMidiInput&) = delete;
  CoreMidiInput& operator=(const CoreMidiInput&) = delete;

  /// CONTROL thread: create the MIDI client + input port and connect to the
  /// source endpoint at `source_index` (CoreMIDI source ordering). Returns false
  /// if there is no such source or the port could not be created.
  bool open(size_t source_index);

  /// CONTROL thread: number of CoreMIDI source endpoints currently present.
  static size_t source_count();

  /// CONTROL thread: attach CoreAudioDevice::midi_time_mapper() (or another
  /// mapper driven by the active audio backend). Not owned; it must outlive
  /// this input while connected.
  void set_time_mapper(const MidiHostTimeMapper* mapper) noexcept;

  /// HOST callback thread: enqueue a UMP tagged with a monotonic host time in
  /// nanoseconds. When a mapper is attached the event is converted to an
  /// absolute render frame before entering the RT queue; otherwise it falls
  /// back to the next block start.
  bool push_event_at_host_time(const midi::Ump& ump, uint64_t host_time_ns) noexcept;

  /// CONTROL thread: disconnect and tear down the port/client.
  void close() noexcept;

  // MidiInputSource — delegate to the internal fixed buffer.
  bool push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept override;
  size_t drain(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame) noexcept override;
  size_t drain_block(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                     int num_frames) noexcept override;
  size_t pending_count() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Live CoreMIDI output: the runtime sends UMP events (RT-safe) which a
/// control/port thread flushes to the destination endpoint via MIDISendEventList.
class CoreMidiOutput final : public MidiOutputSink {
 public:
  CoreMidiOutput();
  ~CoreMidiOutput() override;

  CoreMidiOutput(const CoreMidiOutput&) = delete;
  CoreMidiOutput& operator=(const CoreMidiOutput&) = delete;

  /// CONTROL thread: create the MIDI client + output port and connect to the
  /// destination endpoint at `destination_index`. Returns false on failure.
  bool open(size_t destination_index);

  /// CONTROL thread: number of CoreMIDI destination endpoints currently present.
  static size_t destination_count();

  /// CONTROL thread: attach the same mapper the audio backend publishes. Output
  /// MidiEvent::render_frame values are converted to CoreMIDI host timestamps
  /// during flush_output(). Not owned; must outlive this sink while connected.
  void set_time_mapper(const MidiHostTimeMapper* mapper) noexcept;

  void close() noexcept;

  /// CONTROL thread: attach the payload store used to resolve SysEx-handle UMPs
  /// to wire bytes during flush_output(). Not owned; must outlive this sink. When
  /// unset (or a handle is unknown), SysEx events are skipped rather than sent.
  void set_sysex_store(const midi::SysExStore* store) noexcept;

  /// CONTROL / port thread (NOT the audio thread): drain queued events and write
  /// them to the device via MIDISendEventList. SysEx-handle UMPs are resolved
  /// through the attached SysExStore (see set_sysex_store) and expanded into
  /// bounded SysEx7 chunks. A failed CoreMIDI write retains the current event
  /// and its unsent SysEx packet offset for the next call. Returns the number
  /// of source events completely flushed.
  size_t flush_output() noexcept;

  /// CONTROL thread telemetry. send_error_count counts failed CoreMIDI writes;
  /// invalid_sysex_count counts missing handles and payloads that cannot be
  /// represented as SysEx7. Counters are cumulative for this instance.
  uint32_t send_error_count() const noexcept;
  uint32_t invalid_sysex_count() const noexcept;

  // MidiOutputSink — delegate to the internal fixed queue (RT-safe).
  bool send(const midi::MidiEvent& event) noexcept override;
  size_t queued_count() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::host::backends
