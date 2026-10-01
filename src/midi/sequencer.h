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
#include <memory>
#include <utility>
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
  /// the audio thread; must not allocate. For SysEx, `sysex_payload` and
  /// `prepared_sysex` are borrowed views valid only for this callback. Consume
  /// them synchronously and never retain either pointer; a published clip bank
  /// keeps them alive through the callback, while live/direct callers own their
  /// corresponding storage.
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
  ///
  /// Every event's Ump::group is re-derived from its own word[0] before the set
  /// is published, so a caller that carries the group out of band cannot leave
  /// the cached field contradicting the wire form. This is the single place the
  /// rule lives: the C ABI, the WASM wrappers and the arrangement compiler all
  /// reach the sequencer through here.
  void set_midi_clips(std::vector<MidiClipSchedule> clips);

  /// CONTROL thread: install / replace a live MIDI FX insert for one
  /// destination. The audio thread applies it immediately before dispatching to
  /// the sink, so scheduled clips stay unmodified and the insert can be changed
  /// independently of clip content. Returns false when the fixed destination
  /// insert table is full. Not RT-safe.
  bool set_midi_fx(uint32_t destination_id, const MidiFxChain& chain,
                   int64_t render_frame = 0) noexcept;
  void clear_midi_fx(uint32_t destination_id) noexcept;

  /// AUDIO thread: adopt pending FX configuration at a block boundary. Any
  /// replaced/removed destination is flushed to the sink before the new chain
  /// becomes active, so generated notes and pending events cannot hang.
  void acquire_midi_fx(int64_t render_frame) noexcept;

  /// AUDIO thread: adopt the latest published clip set. Call once at block
  /// start before process_block. RT-safe, no alloc.
  void acquire_midi_clips() noexcept { clips_.acquire(); }

  /// CONTROL thread between audio blocks: drain and adopt every accepted clip
  /// publication, including a replacement waiting in the publisher's pending
  /// slot. This is stronger than one audio-boundary acquire and is required
  /// when a control transaction must make its prepared clip bank current before
  /// it returns.
  void acquire_midi_clips_control_quiescent() noexcept { clips_.acquire_control_quiescent(); }

  /// AUDIO thread: the clip set most recently adopted by acquire_midi_clips(),
  /// or nullptr before any set has been published. Lets a caller (the
  /// per-destination gain/fade envelope, midi_clip_envelope.h) read the same
  /// snapshot process_block() scans without re-deriving or copying it.
  const std::vector<MidiClipSchedule>* current_clips() const noexcept { return clips_.current(); }

  /// CONTROL thread: the newest accepted clip set, including a publication that
  /// is still waiting in the RtPublisher hand-off ring. The returned owner is
  /// copied by control-thread callers that need to stage a replacement; this
  /// accessor must not be called concurrently with another control publisher.
  const std::shared_ptr<const std::vector<MidiClipSchedule>>& control_clips() const noexcept {
    return clips_.control_current();
  }

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

  /// AUDIO thread: dispatch a single host-injected (live) SysEx event to a
  /// destination at `render_frame`. `sysex_payload`/`sysex_payload_size` view
  /// control-thread-owned bytes that must outlive the dispatch (the engine's
  /// bounded SysEx payload store keeps them valid until the instrument consumes
  /// them synchronously). `prepared_sysex`, when non-null, is the corresponding
  /// immutable control-thread operation retained by the caller. Routed through
  /// the same process_event path as clip SysEx, so it bypasses MIDI FX and is
  /// dispatched synchronously. RT-safe, no alloc.
  void inject_event(uint32_t destination_id, int64_t render_frame, const Ump& ump,
                    const uint8_t* sysex_payload, size_t sysex_payload_size,
                    const PreparedMidiSysEx* prepared_sysex = nullptr) noexcept;

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
  ///
  /// An unprepared set holds kCapacity offsets inline; prepare() reserves a
  /// larger table on CONTROL so the audio collector never grows it.
  class BoundaryOffsets {
   public:
    static constexpr size_t kCapacity = 64;

    /// CONTROL thread: reserve room for @p capacity offsets (at least
    /// kCapacity). A failed allocation keeps the previous storage.
    void prepare(size_t capacity);
    size_t capacity() const noexcept { return capacity_; }
    size_t size() const noexcept { return size_; }
    bool overflowed() const noexcept { return overflowed_; }
    /// Offsets ascend; @p index must be below size().
    int operator[](size_t index) const noexcept { return offsets()[index]; }

   private:
    friend class MidiSequencer;

    int* offsets() noexcept {
      return capacity_ > kCapacity ? prepared_offsets_.data() : inline_offsets_.data();
    }
    const int* offsets() const noexcept {
      return capacity_ > kCapacity ? prepared_offsets_.data() : inline_offsets_.data();
    }
    uint8_t* seen() noexcept {
      return capacity_ > kCapacity ? prepared_seen_.data() : inline_seen_.data();
    }
    // AUDIO thread: empty the set, resetting only the marks the last block set.
    void clear() noexcept;
    // AUDIO thread: insert @p offset in order unless already present.
    void push(int offset) noexcept;

    std::array<int, kCapacity> inline_offsets_{};
    // Per-offset deduplication marks; an offset at or past capacity_ is found
    // by a scan instead.
    std::array<uint8_t, kCapacity> inline_seen_{};
    std::vector<int> prepared_offsets_;
    std::vector<uint8_t> prepared_seen_;
    size_t capacity_ = kCapacity;
    size_t size_ = 0;
    bool overflowed_ = false;
  };
  void collect_boundaries(int64_t block_start_frame, int num_frames,
                          BoundaryOffsets* out) const noexcept;

 private:
  struct ActiveNote {
    uint8_t group = 0;
    uint8_t channel = 0;
    uint8_t note = 0;
    uint32_t destination_id = 0;
    uint32_t source_track_id = 0;
    uint32_t clip_id = 0;
    bool from_clip = false;
  };
  struct DestinationFx {
    uint32_t destination_id = 0;
    uint64_t generation = 0;
    bool active = false;
    MidiFxChain chain;
    MidiFxBuffer buffer;
    size_t next_input_ordinal = 0;
  };
  struct DestinationFxConfig {
    uint32_t destination_id = 0;
    uint64_t generation = 0;
    bool active = false;
    TransposeConfig transpose{};
    QuantizeConfig quantize{};
    VelocityCurveConfig velocity{};
    ChordConfig chord{};
    ArpeggiatorConfig arpeggiator{};
    HumanizeConfig humanize{};
  };
  struct MidiFxSnapshot {
    std::array<DestinationFxConfig, kMaxMidiFxInserts> destinations{};
  };
  struct PendingFxEvent {
    uint32_t destination_id = 0;
    MidiEvent event;
    uint32_t clip_id = 0;
    bool from_clip = false;
  };
  // The MIDI FX work buffers are large enough that keeping them inline makes
  // every engine instance unnecessarily expensive to construct on the stack.
  // They are allocated once by prepare() on the control thread and then only
  // dereferenced by the audio thread. Keeping the owner stable across reset()
  // and repeated prepare() calls makes the RT path allocation-free while also
  // avoiding a control/audio lifetime race.
  struct RuntimeStorage {
    std::array<DestinationFx, kMaxMidiFxInserts> midi_fx{};
    std::array<PendingFxEvent, kMaxPendingFxEvents> pending_fx{};
  };

  // Records a sounding note; returns false on capacity overflow (bumps counter).
  bool track_note_on(uint8_t group, uint8_t channel, uint8_t note, uint32_t destination_id,
                     uint32_t source_track_id, bool from_clip, uint32_t clip_id) noexcept;
  // Removes a sounding note if present. Keyed by destination_id too, so a
  // note-off on one destination never releases an identically-pitched note
  // sounding on a different destination/instrument.
  void track_note_off(uint8_t group, uint8_t channel, uint8_t note, uint32_t destination_id,
                      uint32_t source_track_id, bool from_clip, uint32_t clip_id) noexcept;
  void dispatch(uint32_t destination_id, const MidiEvent& event) noexcept;
  // Emit the standard reset sequence (damper off, reset-all-controllers,
  // all-notes-off, pitch-bend center) for one channel at render_frame. Used on a
  // playback discontinuity so a note released under a held sustain pedal does not
  // keep ringing and stale pitch-bend / CC state does not carry across.
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
  void dispatch_pending_through(int64_t block_start_frame, int64_t block_end_frame,
                                int64_t through_frame) noexcept;
  void clear_pending_for_destination(uint32_t destination_id) noexcept;
  void clear_pending_for_clip(uint32_t clip_id) noexcept;
  void release_notes_for_clip(uint32_t clip_id, int64_t render_frame,
                              bool clear_pending = true) noexcept;
  // Release note-offs for every sounding note (and drop pending FX events) whose
  // source clip is no longer present in `clips` (nullptr = empty set). Called
  // once when the published clip set changes so a live mute / clip delete that
  // recompiles and republishes without a clip does not hang its notes.
  void release_notes_for_absent_clips(const std::vector<MidiClipSchedule>* clips,
                                      int64_t render_frame) noexcept;

  double sample_rate_ = constants::kDefaultDawSampleRate;
  MidiEventSink* sink_ = nullptr;
  mutable rt::RtPublisher<std::vector<MidiClipSchedule>> clips_;
  std::atomic<size_t> clip_count_{0};
  // Audio-thread-only: the clip snapshot seen by the previous process_block, used
  // only for identity comparison to detect a republished set (never dereferenced
  // after the pointer goes stale, so comparing a freed value is safe).
  const std::vector<MidiClipSchedule>* last_clips_ = nullptr;

  // Fixed-capacity active-note table (audio thread only).
  std::array<ActiveNote, kMaxActiveNotes> active_{};
  size_t active_count_ = 0;
  std::atomic<uint32_t> active_note_overflow_count_{0};
  std::atomic<uint32_t> dispatched_event_count_{0};
  mutable rt::RtPublisher<MidiFxSnapshot> midi_fx_snapshots_;
  const MidiFxSnapshot* last_midi_fx_snapshot_ = nullptr;
  uint64_t next_midi_fx_generation_ = 1;  // control-thread only
  std::unique_ptr<RuntimeStorage> runtime_storage_;
  size_t pending_fx_count_ = 0;
  std::atomic<uint32_t> midi_fx_pending_overflow_count_{0};
};

}  // namespace sonare::midi
