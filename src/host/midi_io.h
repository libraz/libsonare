#pragma once

/// @file midi_io.h
/// @brief Live MIDI input / output seams: abstract interfaces that
///        exchange midi::Ump / midi::MidiEvent fixed records with the MIDI
///        runtime — NEVER raw OS handles. Header-only.
///
/// Scope and invariants
/// --------------------
///  - These seams trade in core types only: a midi::Ump payload and a
///    render-frame-timestamped midi::MidiEvent. They include NO OS MIDI headers
///    (CoreMIDI / ALSA / Windows MM); the live port lives out-of-tree / behind
///    a build option (invariant 6). The descriptor for a port is the existing
///    data-only midi::ExternalPortDescriptor (sound_destination.h); this seam
///    is the I/O verb layer over it.
///  - Fixed records, not variable streams: events are exchanged as POD
///    midi::Ump / midi::MidiEvent values that ride RT structures without
///    allocation. SysEx / property data is referenced by handle (per ump.h),
///    never inlined.
///  - Header-only: abstract interfaces, no .cpp, no lib.
///
/// SysEx-handle transfer contract (across this seam)
/// -------------------------------------------------
/// A UMP that carries a SysEx / property payload sets midi::Ump::sysex_handle to
/// a non-zero handle; the bytes live in a control-thread midi::SysExStore. The
/// handle, NOT the bytes, crosses this seam in both directions:
///  - INPUT: when push_event() enqueues a UMP with a non-zero sysex_handle, the
///    HANDLE NAMESPACE is the host's own (the host owns the store its live port
///    parsed the incoming SysEx into). The runtime, on drain(), treats the
///    handle as opaque and forwards it unchanged; it does NOT dereference the
///    payload on the audio thread (no SysExStore lookup, no variable-length
///    copy). A consumer that needs the bytes resolves them on the control thread
///    against the host's store.
///  - OUTPUT: when the runtime send()s a UMP with a non-zero sysex_handle, the
///    handle is valid in the RUNTIME's store; the host resolves the payload off
///    the audio thread (in its port-flush thread) before writing it to the
///    device. send() copies only the fixed UMP record — it never inlines or
///    allocates the payload.
/// In both directions the payload bytes are an opaque byte span owned by the
/// originating side and are NEVER copied on the audio thread; only the
/// fixed-size handle travels through the RT structures (invariant 6). Handles
/// from one side are not meaningful in the other side's store, so a host that
/// loops MIDI input back to output must re-resolve and re-register the payload
/// on the control thread rather than forwarding the raw handle.
///
/// Threading / RT contract
/// -----------------------
///  - INPUT (MidiInputSource): the host's port thread pushes incoming events
///    into the source (push_event), which buffers them; the RT runtime DRAINS
///    them at block start into a caller-owned fixed array (drain), exactly like
///    midi::capture. drain() is RT-safe (no alloc, no lock-wait, no I/O);
///    push_event() runs on the host's MIDI-callback thread.
///  - OUTPUT (MidiOutputSink): the RT runtime SENDS events (send) to the sink,
///    which the host's port thread flushes to the live port. send() is RT-safe;
///    the actual device write happens off the audio thread.
///
/// MPE I/O seam and MPE / SMF fidelity
/// -----------------------------------
/// This seam is UMP-native, so MPE (MIDI Polyphonic Expression) and full MIDI
/// 2.0 per-note expression pass through LOSSLESSLY as fixed midi::Ump records:
///  - MIDI 2.0 per-note pitch / per-note controllers / per-note attributes ride
///    in the UMP word fields directly; per-note channel/group routing is
///    preserved on both push_event() and send().
///  - MPE expressed in MIDI 1.0 form (per-voice channel spread across an MPE
///    zone, with per-channel pitch-bend / CC#74 / channel-pressure) is carried
///    as MIDI-1.0-typed UMPs; this seam does NOT collapse the zone or remap
///    member channels — the host's port owns MPE zone configuration. The seam
///    neither imposes nor enforces a zone layout; it forwards the channel as-is.
/// FIDELITY LIMITS:
///  - This seam carries individual events only; it has no MPE-zone model and
///    performs no MPE<->single-channel conversion. Down-converting MIDI 2.0
///    per-note expression to MIDI 1.0 MPE (or vice versa) is the host's job
///    outside this seam (see midi::midi2_to_midi1 for the lossy mapping).
///  - SMF (Standard MIDI File) fidelity is governed by midi/smf.{h,cpp}, NOT by
///    this live-I/O seam: an SMF round-trip preserves channel-voice events,
///    markers, time-signature metronome bytes and merges multi-packet SysEx, but
///    MIDI 2.0-only per-note forms that have no MIDI 1.0 SMF encoding are counted
///    in SmfExportResult::skipped_events rather than silently dropped. Use the
///    MIDI 2.0 clip container (midi/smf2.{h,cpp}) for lossless MIDI 2.0 / MPE
///    persistence. This seam is real-time transport only and does no file I/O.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host {

/// Live MIDI INPUT seam. A host port implementation buffers incoming UMP from
/// the device and the runtime drains it as fixed midi::MidiEvent records,
/// stamping each with the render frame the runtime assigns. No OS handle is
/// exposed; the host owns the device behind this interface.
class MidiInputSource {
 public:
  virtual ~MidiInputSource() = default;

  /// HOST MIDI-callback thread: enqueue one incoming UMP, tagged with the
  /// `port_time_samples` the host estimates for it (used to align to render
  /// frames on drain). Returns false if the internal fixed buffer overflowed
  /// (the event is dropped; the host may surface telemetry). MUST NOT allocate.
  virtual bool push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept = 0;

  /// AUDIO/RT thread: drain up to `capacity` buffered events into `out` as
  /// render-frame-stamped fixed records and return the count written. RT-safe:
  /// no allocation, no lock-wait, no I/O. `block_start_frame` is the first
  /// render frame of the current block so the source can map port time to an
  /// in-block render_frame. Drained events are removed from the buffer.
  virtual size_t drain(midi::MidiEvent* out, size_t capacity,
                       int64_t block_start_frame) noexcept = 0;

  /// AUDIO/RT thread: block-size-aware drain. Default preserves compatibility
  /// with older implementations by calling drain(), then clamping timestamps to
  /// [block_start_frame, block_start_frame + num_frames). Implementations may
  /// override for tighter behavior. Events are removed from the buffer.
  virtual size_t drain_block(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                             int num_frames) noexcept {
    if (num_frames <= 0) return 0;
    const size_t n = drain(out, capacity, block_start_frame);
    const int64_t block_end_frame = block_start_frame + num_frames;
    for (size_t i = 0; i < n; ++i) {
      if (out[i].render_frame < block_start_frame) out[i].render_frame = block_start_frame;
      if (out[i].render_frame >= block_end_frame) out[i].render_frame = block_end_frame - 1;
    }
    return n;
  }

  /// Number of events currently buffered (lock-free poll). Advisory.
  virtual size_t pending_count() const noexcept = 0;
};

/// Live MIDI OUTPUT seam. The runtime sends UMP events to this sink; the host
/// implementation queues them and flushes to the live port off the audio
/// thread. No OS handle is exposed.
class MidiOutputSink {
 public:
  virtual ~MidiOutputSink() = default;

  /// AUDIO/RT thread: send one event, sample-accurately at `event.render_frame`.
  /// Returns false if the internal fixed queue overflowed (event dropped).
  /// RT-safe: no allocation, no lock-wait, no I/O. The host's port thread
  /// flushes queued events to the device later.
  virtual bool send(const midi::MidiEvent& event) noexcept = 0;

  /// AUDIO/RT thread: convenience overload sending a bare UMP at `render_frame`.
  virtual bool send_ump(const midi::Ump& ump, int64_t render_frame) noexcept {
    return send(midi::MidiEvent{render_frame, ump});
  }

  /// Number of events queued for the port but not yet flushed (advisory).
  virtual size_t queued_count() const noexcept = 0;
};

/// Header-only fixed-capacity MIDI input buffer. This is a concrete seam
/// implementation suitable for tests, embedded hosts, and simple backends that
/// already call push_event() from a single MIDI callback thread and drain() from
/// the audio thread. It is single-producer/single-consumer by contract; no heap
/// allocation after construction.
template <size_t Capacity>
class FixedMidiInputSource final : public MidiInputSource {
 public:
  static_assert(Capacity > 0, "FixedMidiInputSource capacity must be positive");

  bool push_event(const midi::Ump& ump, int64_t port_time_samples) noexcept override {
    const size_t write = write_index_.load(std::memory_order_relaxed);
    const size_t next = increment(write);
    if (next == read_index_.load(std::memory_order_acquire)) {
      dropped_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    buffer_[write] = Slot{ump, port_time_samples};
    write_index_.store(next, std::memory_order_release);
    return true;
  }

  size_t drain(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame) noexcept override {
    if (out == nullptr || capacity == 0) {
      return 0;
    }
    size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    size_t n = 0;
    while (read != write && n < capacity) {
      const int64_t offset =
          buffer_[read].port_time_samples < 0 ? 0 : buffer_[read].port_time_samples;
      out[n].render_frame = block_start_frame + offset;
      out[n].ump = buffer_[read].ump;
      read = increment(read);
      ++n;
    }
    read_index_.store(read, std::memory_order_release);
    for (size_t i = 1; i < n; ++i) {
      midi::MidiEvent value = out[i];
      size_t j = i;
      while (j > 0 && out[j - 1].render_frame > value.render_frame) {
        out[j] = out[j - 1];
        --j;
      }
      out[j] = value;
    }
    return n;
  }

  size_t pending_count() const noexcept override {
    return distance(read_index_.load(std::memory_order_acquire),
                    write_index_.load(std::memory_order_acquire));
  }

  uint32_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }

  void reset_telemetry() noexcept { dropped_count_.store(0, std::memory_order_relaxed); }

 private:
  static constexpr size_t kSlots = Capacity + 1;

  struct Slot {
    midi::Ump ump{};
    int64_t port_time_samples = 0;
  };

  static constexpr size_t increment(size_t index) noexcept { return (index + 1) % kSlots; }

  static constexpr size_t distance(size_t read, size_t write) noexcept {
    return write >= read ? write - read : kSlots - read + write;
  }

  std::array<Slot, kSlots> buffer_{};
  std::atomic<size_t> read_index_{0};
  std::atomic<size_t> write_index_{0};
  std::atomic<uint32_t> dropped_count_{0};
};

/// Header-only fixed-capacity MIDI output queue. send() is RT-safe and only
/// copies fixed midi::MidiEvent records. A host/device thread drains queued
/// events with drain_queued() and performs actual OS MIDI writes outside core.
template <size_t Capacity>
class FixedMidiOutputSink final : public MidiOutputSink {
 public:
  static_assert(Capacity > 0, "FixedMidiOutputSink capacity must be positive");

  bool send(const midi::MidiEvent& event) noexcept override {
    const size_t write = write_index_.load(std::memory_order_relaxed);
    const size_t next = increment(write);
    if (next == read_index_.load(std::memory_order_acquire)) {
      dropped_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_[write] = event;
    write_index_.store(next, std::memory_order_release);
    return true;
  }

  size_t queued_count() const noexcept override {
    return distance(read_index_.load(std::memory_order_acquire),
                    write_index_.load(std::memory_order_acquire));
  }

  /// HOST/device thread: drain up to `capacity` queued events into `out`.
  /// Drained events are removed. No allocation.
  size_t drain_queued(midi::MidiEvent* out, size_t capacity) noexcept {
    if (out == nullptr || capacity == 0) {
      return 0;
    }
    size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    size_t n = 0;
    while (read != write && n < capacity) {
      out[n] = queue_[read];
      read = increment(read);
      ++n;
    }
    read_index_.store(read, std::memory_order_release);
    return n;
  }

  uint32_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }

  void reset_telemetry() noexcept { dropped_count_.store(0, std::memory_order_relaxed); }

 private:
  static constexpr size_t kSlots = Capacity + 1;

  static constexpr size_t increment(size_t index) noexcept { return (index + 1) % kSlots; }

  static constexpr size_t distance(size_t read, size_t write) noexcept {
    return write >= read ? write - read : kSlots - read + write;
  }

  std::array<midi::MidiEvent, kSlots> queue_{};
  std::atomic<size_t> read_index_{0};
  std::atomic<size_t> write_index_{0};
  std::atomic<uint32_t> dropped_count_{0};
};

}  // namespace sonare::host
