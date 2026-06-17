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
#include <memory>

#include "host/midi_io.h"

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

  /// CONTROL thread: disconnect and tear down the port/client.
  void close() noexcept;

  // MidiInputSource — delegate to the internal fixed buffer.
  bool push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept override;
  size_t drain(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame) noexcept override;
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

  void close() noexcept;

  /// CONTROL / port thread (NOT the audio thread): drain queued events and write
  /// them to the device via MIDISendEventList. Returns the number flushed.
  size_t flush_output() noexcept;

  // MidiOutputSink — delegate to the internal fixed queue (RT-safe).
  bool send(const midi::MidiEvent& event) noexcept override;
  size_t queued_count() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::host::backends
