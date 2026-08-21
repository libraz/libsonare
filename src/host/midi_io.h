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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace sonare::host {

/// Lock-free correlation between a monotonic host clock and the engine's
/// absolute render-frame timeline. A device backend publishes an anchor at the
/// first frame of each audio callback; timestamped MIDI backends read the latest
/// complete anchor to map both input host times and output render frames.
///
/// The mapper is single-writer/multi-reader. publish_anchor() is RT-safe and
/// performs only bounded arithmetic plus atomic stores. Readers never observe a
/// partially-published tuple.
class MidiHostTimeMapper {
 public:
  void reset() noexcept {
    sequence_.fetch_add(1u, std::memory_order_acq_rel);
    host_time_ns_.store(0, std::memory_order_relaxed);
    render_frame_.store(0, std::memory_order_relaxed);
    sample_rate_millihz_.store(0, std::memory_order_relaxed);
    sequence_.fetch_add(1u, std::memory_order_release);
  }

  void publish_anchor(uint64_t host_time_ns, int64_t render_frame, double sample_rate) noexcept {
    if (host_time_ns == 0 || !std::isfinite(sample_rate) || sample_rate <= 0.0) return;
    const long double scaled_rate = static_cast<long double>(sample_rate) * 1000.0L;
    if (scaled_rate > static_cast<long double>(std::numeric_limits<uint64_t>::max())) return;
    const uint64_t rate_millihz = static_cast<uint64_t>(scaled_rate + 0.5L);
    if (rate_millihz == 0) return;

    sequence_.fetch_add(1u, std::memory_order_acq_rel);
    host_time_ns_.store(host_time_ns, std::memory_order_relaxed);
    render_frame_.store(render_frame, std::memory_order_relaxed);
    sample_rate_millihz_.store(rate_millihz, std::memory_order_relaxed);
    sequence_.fetch_add(1u, std::memory_order_release);
  }

  bool host_time_to_render_frame(uint64_t host_time_ns, int64_t* out) const noexcept {
    if (out == nullptr) return false;
    Snapshot snapshot;
    if (!read_snapshot(&snapshot)) return false;
    const long double delta_ns =
        static_cast<long double>(host_time_ns) - static_cast<long double>(snapshot.host_time_ns);
    const long double delta_frames =
        delta_ns * static_cast<long double>(snapshot.sample_rate_millihz) / 1.0e12L;
    const long double frame = static_cast<long double>(snapshot.render_frame) + delta_frames;
    if (frame < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        frame > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    *out = static_cast<int64_t>(std::llround(frame));
    return true;
  }

  bool render_frame_to_host_time(int64_t render_frame, uint64_t* out) const noexcept {
    if (out == nullptr) return false;
    Snapshot snapshot;
    if (!read_snapshot(&snapshot)) return false;
    const long double delta_frames =
        static_cast<long double>(render_frame) - static_cast<long double>(snapshot.render_frame);
    const long double delta_ns =
        delta_frames * 1.0e12L / static_cast<long double>(snapshot.sample_rate_millihz);
    const long double host_time = static_cast<long double>(snapshot.host_time_ns) + delta_ns;
    if (host_time <= 0.0L ||
        host_time > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
      return false;
    }
    *out = static_cast<uint64_t>(host_time + 0.5L);
    return true;
  }

 private:
  struct Snapshot {
    uint64_t host_time_ns = 0;
    int64_t render_frame = 0;
    uint64_t sample_rate_millihz = 0;
  };

  bool read_snapshot(Snapshot* out) const noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t begin = sequence_.load(std::memory_order_acquire);
      if ((begin & 1u) != 0u) continue;
      Snapshot snapshot;
      snapshot.host_time_ns = host_time_ns_.load(std::memory_order_relaxed);
      snapshot.render_frame = render_frame_.load(std::memory_order_relaxed);
      snapshot.sample_rate_millihz = sample_rate_millihz_.load(std::memory_order_relaxed);
      // The three loads above must complete BEFORE the closing sequence read,
      // or the check below validates a counter that was read too early to cover
      // them. An acquire on the load itself does not do this: acquire orders
      // what follows it, not what precedes it, so on a weakly ordered target
      // (arm64, which is the platform these backends exist for) a data load may
      // still be issued after it and pick up the next anchor's value while the
      // counters compare equal. The fence is the half that seqlock needs: it
      // keeps the prior loads on this side of the comparison, which is what
      // makes "all three fields came from one publish" true rather than merely
      // plausible under sequential reasoning.
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t end = sequence_.load(std::memory_order_relaxed);
      if (begin == end && snapshot.host_time_ns != 0 && snapshot.sample_rate_millihz != 0) {
        *out = snapshot;
        return true;
      }
    }
    return false;
  }

  std::atomic<uint32_t> sequence_{0};
  std::atomic<uint64_t> host_time_ns_{0};
  std::atomic<int64_t> render_frame_{0};
  std::atomic<uint64_t> sample_rate_millihz_{0};
};

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
  ///
  /// Single-producer invariant: at most one thread may call push_event() (or
  /// any other producer-side entry point this implementation exposes) at a
  /// time. An implementation that also owns an internal callback thread of its
  /// own (e.g. a live OS MIDI port) MUST NOT let this override run concurrently
  /// with that thread — either by construction (the implementation is only
  /// reachable before/after the internal producer is active) or by rejecting
  /// (returning false) while the internal producer is live. Two concurrent
  /// producers racing on the same underlying buffer is undefined behavior, not
  /// merely a lost event.
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
    return enqueue(ump, port_time_samples, false);
  }

  /// HOST callback thread: enqueue an event whose timestamp is already mapped
  /// to the engine's absolute render-frame timeline.
  bool push_event_at_render_frame(const midi::Ump& ump, int64_t render_frame) noexcept {
    return enqueue(ump, render_frame, true);
  }

  size_t drain(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame) noexcept override {
    if (out == nullptr || capacity == 0) {
      return 0;
    }
    size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    size_t n = 0;
    while (read != write && n < capacity) {
      const Slot& slot = buffer_[read];
      const int64_t offset = slot.time_samples < 0 ? 0 : slot.time_samples;
      // A live input event owns no track lane and no resolved SysEx view, so the
      // slot is reset first: a caller-owned scratch buffer reused across drains
      // would otherwise keep the previous drain's source_track_id and its
      // (by now dangling) sysex_payload pointer.
      out[n] = midi::MidiEvent{};
      out[n].render_frame =
          slot.absolute_render_frame ? slot.time_samples : block_start_frame + offset;
      out[n].ump = slot.ump;
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

  size_t drain_block(midi::MidiEvent* out, size_t capacity, int64_t block_start_frame,
                     int num_frames) noexcept override {
    if (out == nullptr || capacity == 0 || num_frames <= 0) return 0;
    const int64_t block_end_frame = block_start_frame + num_frames;
    size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    size_t n = 0;
    while (read != write && n < capacity) {
      const Slot& slot = buffer_[read];
      if (slot.absolute_render_frame && slot.time_samples >= block_end_frame) {
        // CoreMIDI delivers timestamp-ordered packets. Leave a future event and
        // everything after it queued until the block containing its frame.
        break;
      }
      // Reset the slot before filling it: see drain() for why a reused caller
      // buffer must not inherit the previous drain's non-UMP fields.
      out[n] = midi::MidiEvent{};
      if (slot.absolute_render_frame) {
        out[n].render_frame =
            slot.time_samples < block_start_frame ? block_start_frame : slot.time_samples;
      } else {
        const int64_t offset = slot.time_samples < 0 ? 0 : slot.time_samples;
        out[n].render_frame = block_start_frame + offset;
        if (out[n].render_frame >= block_end_frame) out[n].render_frame = block_end_frame - 1;
      }
      out[n].ump = slot.ump;
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

  /// HOST lifecycle thread: discard queued events after the producer has been
  /// stopped (for example before reopening a MIDI device on a new timeline).
  void clear() noexcept {
    const size_t write = write_index_.load(std::memory_order_acquire);
    read_index_.store(write, std::memory_order_release);
  }

  void reset_telemetry() noexcept { dropped_count_.store(0, std::memory_order_relaxed); }

 private:
  bool enqueue(const midi::Ump& ump, int64_t time_samples, bool absolute_render_frame) noexcept {
    const size_t write = write_index_.load(std::memory_order_relaxed);
    const size_t next = increment(write);
    if (next == read_index_.load(std::memory_order_acquire)) {
      dropped_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    buffer_[write] = Slot{ump, time_samples, absolute_render_frame};
    write_index_.store(next, std::memory_order_release);
    return true;
  }
  static constexpr size_t kSlots = Capacity + 1;

  struct Slot {
    midi::Ump ump{};
    int64_t time_samples = 0;
    bool absolute_render_frame = false;
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

  /// HOST lifecycle thread: discard queued events after the producer stopped.
  void clear() noexcept {
    const size_t write = write_index_.load(std::memory_order_acquire);
    read_index_.store(write, std::memory_order_release);
  }

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

/// Reserved destination id for transport / clock bytes that are not bound to a
/// single track lane. Events tagged with this destination carry a System (UMP
/// message type 0x1) payload — a MIDI 1.0 System Real-Time / Common byte (clock
/// 0xF8, start 0xFA, continue 0xFB, stop 0xFC, song-position 0xF2) — meant for
/// every open external port rather than one track's instrument.
inline constexpr uint32_t kTransportDestination = 0xFFFFFFFFu;

/// A destination-tagged MIDI output event: which lane (Track.midi_destination_id)
/// produced it, plus the render-frame-stamped UMP. Trivially copyable so it can
/// ride the RT output queue. kTransportDestination tags transport/clock bytes.
struct ExternalMidiRecord {
  uint32_t destination_id = 0;
  midi::MidiEvent event{};

  bool operator==(const ExternalMidiRecord& o) const noexcept {
    return destination_id == o.destination_id && event == o.event;
  }
  bool operator!=(const ExternalMidiRecord& o) const noexcept { return !(*this == o); }
};

/// One lowered MIDI 1.0 byte message (1..3 bytes).
struct ExternalMidi1Message {
  uint8_t bytes[3] = {0, 0, 0};
  uint8_t byte_count = 0;
};

/// The MIDI 1.0 messages a single drained ExternalMidiRecord lowers to. A
/// channel-voice UMP yields 1 message, except a MIDI 2.0 program change with
/// bank select which yields up to 3 (two bank-select CCs + the program change).
struct ExternalMidi1Lowered {
  ExternalMidi1Message messages[3] = {};
  uint8_t count = 0;
};

/// @brief Lowers a drained external-MIDI record to MIDI 1.0 byte messages.
/// @details Shared by every host surface (WASM / Node / Python / C ABI) so the
///   lowering rules stay identical. Transport/clock records (destination ==
///   kTransportDestination) yield one single-byte system message. Channel-voice
///   UMPs yield 1..3 messages. UMP types that do not lower to MIDI 1.0 (SysEx /
///   Data, Utility, MIDI-2-only controllers) yield count == 0.
inline ExternalMidi1Lowered lower_external_midi_record(const ExternalMidiRecord& rec) noexcept {
  ExternalMidi1Lowered out{};
  if (rec.destination_id == kTransportDestination) {
    // System real-time / common byte (clock 0xF8 / start / continue / stop).
    out.messages[0].bytes[0] = static_cast<uint8_t>((rec.event.ump.words[0] >> 16) & 0xFFu);
    out.messages[0].byte_count = 1;
    out.count = 1;
    return out;
  }
  midi::Midi1MessageList list{};
  switch (rec.event.ump.message_type()) {
    case midi::UmpMessageType::kMidi1ChannelVoice:
      list.messages[0] = rec.event.ump;
      list.count = 1;
      break;
    case midi::UmpMessageType::kMidi2ChannelVoice:
      list = midi::midi2_to_midi1_messages(rec.event.ump);
      break;
    default:
      return out;  // not lowerable to MIDI 1.0
  }
  for (uint8_t m = 0; m < list.count && out.count < 3; ++m) {
    ExternalMidi1Message& msg = out.messages[out.count];
    const size_t n = midi::ump_to_midi1_bytes(list.messages[m], msg.bytes, sizeof(msg.bytes));
    if (n == 0) continue;
    msg.byte_count = static_cast<uint8_t>(n);
    ++out.count;
  }
  return out;
}

/// Header-only fixed-capacity, destination-tagged MIDI output queue. Unlike
/// MidiOutputSink (a single merged stream), this preserves the originating
/// destination so a host can route each track's MIDI to a different external
/// port. send() is RT-safe (audio thread); a host/control thread drains queued
/// records with drain(). Single-producer/single-consumer by contract; no heap
/// allocation after construction.
template <size_t Capacity>
class FixedExternalMidiOutputQueue {
 public:
  static_assert(Capacity > 0, "FixedExternalMidiOutputQueue capacity must be positive");

  /// AUDIO/RT thread: enqueue one destination-tagged event. Returns false if the
  /// fixed queue overflowed (event dropped, dropped_count() bumped). RT-safe: no
  /// allocation, no lock-wait, no I/O.
  bool send(uint32_t destination_id, const midi::MidiEvent& event) noexcept {
    const size_t write = write_index_.load(std::memory_order_relaxed);
    const size_t next = increment(write);
    if (next == read_index_.load(std::memory_order_acquire)) {
      dropped_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_[write] = ExternalMidiRecord{destination_id, event};
    write_index_.store(next, std::memory_order_release);
    return true;
  }

  /// HOST/control thread: drain up to `capacity` records into `out`. Drained
  /// records are removed. No allocation. Returns the count written.
  size_t drain(ExternalMidiRecord* out, size_t capacity) noexcept {
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

  size_t pending_count() const noexcept {
    return distance(read_index_.load(std::memory_order_acquire),
                    write_index_.load(std::memory_order_acquire));
  }

  uint32_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }

  void reset_telemetry() noexcept { dropped_count_.store(0, std::memory_order_relaxed); }

 private:
  static constexpr size_t kSlots = Capacity + 1;

  static constexpr size_t increment(size_t index) noexcept { return (index + 1) % kSlots; }

  static constexpr size_t distance(size_t read, size_t write) noexcept {
    return write >= read ? write - read : kSlots - read + write;
  }

  std::array<ExternalMidiRecord, kSlots> queue_{};
  std::atomic<size_t> read_index_{0};
  std::atomic<size_t> write_index_{0};
  std::atomic<uint32_t> dropped_count_{0};
};

}  // namespace sonare::host
