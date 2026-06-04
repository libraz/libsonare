#pragma once

/// @file sequencer.h
/// @brief RT-safe MIDI sequencer: scans compiled MIDI clips per block and
///        dispatches sample-accurate UMP events to a destination sink, tracking
///        sounding notes so loop/seek/stop/clip-end never hang a note.
///
/// Threading / RT contract (mirrors engine::ClipPlayer / AutomationEngine)
/// -----------------------------------------------------------------------
///  - CONTROL thread: set_midi_clips(std::vector<MidiClipSchedule>) publishes a
///    new clip set through an rt::RtPublisher. May allocate; not RT-safe.
///  - AUDIO thread: acquire_midi_clips() once at block start adopts the latest
///    published set, then process_block() scans the block's render-frame range
///    and dispatches events to the sink. The audio path performs ZERO heap
///    allocation, takes NO lock, does NO I/O and NO parsing. The active-note
///    table is a fixed-capacity std::array; capacity overflow is surfaced via an
///    atomic telemetry counter, never by growing.
///
/// Hang-note safety
/// ----------------
/// On loop wrap, seek, stop, clip end, and destination swap the sequencer emits
/// note-off for every currently-sounding note before (or instead of) advancing,
/// so no note is left hanging. After all_notes_off() the active-note count is 0.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/midi_fx.h"
#include "rt/rt_publisher.h"
#include "util/constants.h"

namespace sonare::midi {

/// Destination abstraction the sequencer dispatches events to. Implementations
/// must be RT-safe (no allocation / lock / I/O). A test sink or a null
/// destination is sufficient when no host-instrument route is configured.
class MidiEventSink {
 public:
  virtual ~MidiEventSink() = default;
  /// Receives one dispatched event at sample-accurate `render_frame`. Called on
  /// the audio thread; must not allocate.
  virtual void on_event(uint32_t destination_id, const MidiEvent& event) noexcept = 0;
};

/// A null sink that discards events (default destination before host routing).
class NullMidiEventSink final : public MidiEventSink {
 public:
  void on_event(uint32_t, const MidiEvent&) noexcept override {}
};

class MidiSequencer {
 public:
  /// Maximum simultaneously-sounding notes the active-note table can track.
  /// Sized for dense polyphony across 16 channels; overflow is surfaced via
  /// active_note_overflow_count(), never by allocation. Measured headroom:
  /// 256 voices comfortably covers multi-channel orchestral MIDI within one
  /// block while staying small enough to live inline in the engine.
  static constexpr size_t kMaxActiveNotes = 256;
  static constexpr size_t kMaxMidiFxInserts = 32;
  static constexpr size_t kMaxPendingFxEvents = 512;

  void prepare(double sample_rate);
  void reset() noexcept;

  void set_sink(MidiEventSink* sink) noexcept { sink_ = sink; }

  /// CONTROL thread: publish a new compiled MIDI clip set. May allocate.
  void set_midi_clips(std::vector<MidiClipSchedule> clips);

  /// CONTROL thread: install / replace a live MIDI FX insert for one
  /// destination. The audio thread applies it immediately before dispatching to
  /// the sink, so scheduled clips stay unmodified and the insert can be changed
  /// independently of clip content. Returns false when the fixed destination
  /// insert table is full. Not RT-safe.
  bool set_midi_fx(uint32_t destination_id, const MidiFxChain& chain,
                   int64_t render_frame = 0) noexcept;
  void clear_midi_fx(uint32_t destination_id) noexcept;

  /// AUDIO thread: adopt the latest published clip set. Call once at block
  /// start before process_block. RT-safe, no alloc.
  void acquire_midi_clips() noexcept { clips_.acquire(); }

  /// AUDIO thread: dispatch every event whose render frame falls in
  /// [block_start_frame, block_start_frame + num_frames). RT-safe, no alloc.
  void process_block(int64_t block_start_frame, int num_frames) noexcept;

  /// AUDIO thread: emit note-off for every sounding note (hang-note safety on
  /// loop/seek/stop/clip-end/destination-swap), then clear the table. The
  /// note-offs are dispatched at `render_frame`. After this active_note_count()
  /// is 0. RT-safe, no alloc.
  void all_notes_off(int64_t render_frame) noexcept;

  /// AUDIO thread (or CONTROL thread between blocks): emit note-off for every
  /// note currently sounding on `destination_id` and remove those entries,
  /// leaving notes on other destinations untouched. Used when a single
  /// instrument is swapped/cleared on its destination so its held notes are
  /// released rather than left hanging. RT-safe, no alloc.
  void all_notes_off_for_destination(uint32_t destination_id, int64_t render_frame) noexcept;

  /// AUDIO thread: dispatch a single host-injected (live) UMP event to a
  /// destination, sample-accurately at `render_frame`, maintaining the same
  /// active-note bookkeeping the clip scan uses (so a live note-on can later be
  /// released by all_notes_off and a live note-off clears its entry). This is
  /// the routing path for queueable scalar MIDI commands (e.g. an immediate CC)
  /// that synthesize a UMP outside the compiled clip set. RT-safe, no alloc.
  void inject_event(uint32_t destination_id, int64_t render_frame, const Ump& ump) noexcept;

  /// Number of clips currently scheduled (lock-free poll for the host thread).
  size_t clip_count() const noexcept { return clip_count_.load(std::memory_order_relaxed); }
  /// Number of notes currently sounding (audio-thread state; read for tests).
  size_t active_note_count() const noexcept { return active_count_; }
  uint32_t active_note_overflow_count() const noexcept {
    return active_note_overflow_count_.load(std::memory_order_relaxed);
  }
  uint32_t dispatched_event_count() const noexcept {
    return dispatched_event_count_.load(std::memory_order_relaxed);
  }
  uint32_t midi_fx_pending_overflow_count() const noexcept {
    return midi_fx_pending_overflow_count_.load(std::memory_order_relaxed);
  }

  /// Collect the render-frame offsets of MIDI events in this block as sub-block
  /// boundary candidates (offsets relative to block_start_frame). Mirrors
  /// engine::ClipPlayer::collect_boundaries. RT-safe, no alloc.
  struct BoundaryOffsets {
    static constexpr size_t kCapacity = 64;
    std::array<int, kCapacity> offsets{};
    size_t size = 0;
    bool overflowed = false;
  };
  void collect_boundaries(int64_t block_start_frame, int num_frames,
                          BoundaryOffsets* out) const noexcept;

 private:
  struct ActiveNote {
    uint8_t group = 0;
    uint8_t channel = 0;
    uint8_t note = 0;
    uint32_t destination_id = 0;
    uint32_t clip_id = 0;
    bool from_clip = false;
  };
  struct DestinationFx {
    uint32_t destination_id = 0;
    bool active = false;
    MidiFxChain chain;
    MidiFxBuffer buffer;
  };
  struct PendingFxEvent {
    uint32_t destination_id = 0;
    MidiEvent event;
    uint32_t clip_id = 0;
    bool from_clip = false;
  };

  // Records a sounding note; returns false on capacity overflow (bumps counter).
  bool track_note_on(uint8_t group, uint8_t channel, uint8_t note, uint32_t destination_id,
                     bool from_clip, uint32_t clip_id) noexcept;
  // Removes a sounding note if present. Keyed by destination_id too, so a
  // note-off on one destination never releases an identically-pitched note
  // sounding on a different destination/instrument.
  void track_note_off(uint8_t group, uint8_t channel, uint8_t note, uint32_t destination_id,
                      bool from_clip, uint32_t clip_id) noexcept;
  void dispatch(uint32_t destination_id, const MidiEvent& event) noexcept;
  // Emit the standard reset sequence (damper off, reset-all-controllers,
  // all-notes-off, pitch-bend center) for one channel at render_frame. Used on a
  // playback discontinuity so a note released under a held sustain pedal does not
  // keep ringing and stale pitch-bend / CC state does not carry across (M-4).
  void emit_controller_reset(uint32_t destination_id, uint8_t group, uint8_t channel,
                             int64_t render_frame) noexcept;
  // Emit emit_controller_reset() once per distinct (destination, group, channel)
  // sounding in the active-note table, optionally limited to one destination.
  // Non-mutating; dedup is bounded by kMaxActiveNotes. RT-safe, no alloc.
  void emit_active_controller_resets(bool single_destination, uint32_t destination_id,
                                     int64_t render_frame) noexcept;
  DestinationFx* find_midi_fx(uint32_t destination_id) noexcept;
  const DestinationFx* find_midi_fx(uint32_t destination_id) const noexcept;
  void process_event(uint32_t destination_id, const MidiEvent& event, int64_t block_end_frame,
                     bool from_clip, uint32_t clip_id) noexcept;
  void dispatch_transformed(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                            uint32_t clip_id) noexcept;
  void enqueue_pending(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                       uint32_t clip_id) noexcept;
  void dispatch_pending(int64_t block_start_frame, int64_t block_end_frame) noexcept;
  void clear_pending_for_destination(uint32_t destination_id) noexcept;
  void clear_pending_for_clip(uint32_t clip_id) noexcept;
  void release_notes_for_clip(uint32_t clip_id, int64_t render_frame,
                              bool clear_pending = true) noexcept;

  double sample_rate_ = constants::kDefaultDawSampleRate;
  MidiEventSink* sink_ = nullptr;
  mutable rt::RtPublisher<std::vector<MidiClipSchedule>> clips_;
  std::atomic<size_t> clip_count_{0};

  // Fixed-capacity active-note table (audio thread only).
  std::array<ActiveNote, kMaxActiveNotes> active_{};
  size_t active_count_ = 0;
  std::atomic<uint32_t> active_note_overflow_count_{0};
  std::atomic<uint32_t> dispatched_event_count_{0};
  std::array<DestinationFx, kMaxMidiFxInserts> midi_fx_{};
  std::array<PendingFxEvent, kMaxPendingFxEvents> pending_fx_{};
  size_t pending_fx_count_ = 0;
  std::atomic<uint32_t> midi_fx_pending_overflow_count_{0};
};

}  // namespace sonare::midi
