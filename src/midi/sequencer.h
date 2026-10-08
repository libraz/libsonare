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
///    published set, then dispatch_due() / frames_until_next_event() step through
///    the block event by event (process_block() runs that loop for a caller that
///    renders nothing in between). The audio path performs ZERO heap
///    allocation, takes NO lock, does NO I/O and NO parsing. The active-note
///    table is a fixed-capacity std::array; capacity overflow is surfaced via an
///    atomic telemetry counter, never by growing.
///
/// Hang-note safety
/// ----------------
/// On loop wrap, seek, stop, clip end, and destination swap the sequencer emits
/// note-off for every currently-sounding note before (or instead of) advancing,
/// so no note is left hanging. After all_notes_off() the active-note count is 0.
///
/// Clock basis
/// -----------
/// Compiled clips are stamped on the TIMELINE (which wraps on a loop and jumps on
/// a seek); everything the sink receives is on the monotonic DEVICE clock. A clip
/// event is converted exactly once, as it leaves the clip scan, so MIDI-FX state,
/// pending FX output and every dispatch are device-framed. The two frame types do
/// not convert implicitly, so a comparison across bases does not compile.

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

/// A frame on the transport timeline, the basis compiled clip events are stamped in.
struct TimelineFrame {
  int64_t value = 0;
};

/// A frame on the engine's monotonic device render clock, the basis every
/// dispatched event, live input and pending MIDI-FX output is stamped in.
struct DeviceFrame {
  int64_t value = 0;
};

/// Pairs one device frame with the timeline frame playing at it. While the
/// transport rolls the two advance together; while stopped the timeline holds.
struct SequencerClock {
  DeviceFrame device;
  TimelineFrame timeline;
  bool rolling = true;

  /// A rolling clock whose device and timeline frames coincide (offline and
  /// standalone use).
  static SequencerClock aligned(int64_t frame) noexcept {
    return {DeviceFrame{frame}, TimelineFrame{frame}, true};
  }
  /// The same clock @p frames later.
  SequencerClock advanced(int64_t frames) const noexcept;
};

/// The device frame at which @p frame plays under @p clock.
DeviceFrame timeline_to_device(TimelineFrame frame, const SequencerClock& clock) noexcept;
/// The timeline frame playing at device frame @p frame under @p clock (the held
/// position while stopped).
TimelineFrame device_to_timeline(DeviceFrame frame, const SequencerClock& clock) noexcept;

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
  /// Maximum retained (destination, group, channel) states used to make
  /// controller resets cover channels whose notes have already received a
  /// natural or clip note-off. New stateful events are dropped when this
  /// fixed table is full; semantic note-offs still pass through.
  static constexpr size_t kMaxRetainedChannelStates = 256;
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
  bool set_midi_fx(uint32_t destination_id, const MidiFxChain& chain) noexcept;
  void clear_midi_fx(uint32_t destination_id) noexcept;

  /// AUDIO thread: adopt pending FX configuration at a block boundary. Any
  /// replaced/removed destination is flushed to the sink before the new chain
  /// becomes active, so generated notes and pending events cannot hang.
  void acquire_midi_fx(DeviceFrame render_frame) noexcept;

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

  /// AUDIO thread: dispatch every event due at the clock's device frame -- clip
  /// events at its timeline frame (when rolling), clip and loop-iteration ends
  /// falling there, and pending MIDI-FX output -- then remember the clock as the
  /// basis live injections are quantized against. RT-safe, no alloc.
  void dispatch_due(const SequencerClock& clock) noexcept;

  /// AUDIO thread: frames from the clock's device frame to the next event the
  /// sequencer holds (a clip event, a clip or iteration end, or pending MIDI-FX
  /// output), capped at @p max_frames. Events at the clock frame itself must have
  /// been dispatched first; the result is at least 1 for a positive cap. A host
  /// renders exactly this many frames before the next dispatch_due, so every
  /// event takes effect at its own frame whatever the host block size.
  int frames_until_next_event(const SequencerClock& clock, int max_frames) const noexcept;

  /// AUDIO thread: dispatch every event of the @p num_frames frames starting at
  /// @p clock, each at its own frame (dispatch_due over the span). An event at
  /// the span's exclusive end, a clip-end release included, belongs to the next
  /// call. RT-safe, no alloc.
  void process_block(const SequencerClock& clock, int num_frames) noexcept;

  /// AUDIO thread: emit note-off for every sounding note (hang-note safety on
  /// loop/seek/stop/clip-end/destination-swap), then clear the table and drop
  /// pending clip-originated MIDI-FX output. Pending output of live input keeps
  /// its device frame. After this active_note_count() is 0. RT-safe, no alloc.
  void all_notes_off(DeviceFrame render_frame) noexcept;

  /// AUDIO thread (or CONTROL thread between blocks): emit note-off for every
  /// note currently sounding on `destination_id` and remove those entries,
  /// leaving notes on other destinations untouched. Used when a single
  /// instrument is swapped/cleared on its destination so its held notes are
  /// released rather than left hanging. RT-safe, no alloc.
  void all_notes_off_for_destination(uint32_t destination_id, DeviceFrame render_frame) noexcept;

  /// AUDIO thread: dispatch a single host-injected (live) UMP event to a
  /// destination, sample-accurately at `render_frame`, maintaining the same
  /// active-note bookkeeping the clip scan uses (so a live note-on can later be
  /// released by all_notes_off and a live note-off clears its entry). This is
  /// the routing path for queueable scalar MIDI commands (e.g. an immediate CC)
  /// that synthesize a UMP outside the compiled clip set. A quantizing MIDI-FX
  /// chain snaps it on the timeline grid of the last dispatch_due clock.
  /// RT-safe, no alloc.
  void inject_event(uint32_t destination_id, DeviceFrame render_frame, const Ump& ump) noexcept;

  /// AUDIO thread: dispatch a single host-injected (live) SysEx event to a
  /// destination at `render_frame`. `sysex_payload`/`sysex_payload_size` view
  /// control-thread-owned bytes that must outlive the dispatch (the engine's
  /// bounded SysEx payload store keeps them valid until the instrument consumes
  /// them synchronously). `prepared_sysex`, when non-null, is the corresponding
  /// immutable control-thread operation retained by the caller. Routed through
  /// the same process_event path as clip SysEx, so it bypasses MIDI FX and is
  /// dispatched synchronously. RT-safe, no alloc.
  void inject_event(uint32_t destination_id, DeviceFrame render_frame, const Ump& ump,
                    const uint8_t* sysex_payload, size_t sysex_payload_size,
                    const PreparedMidiSysEx* prepared_sysex = nullptr) noexcept;

  /// Number of clips currently scheduled (lock-free poll for the host thread).
  size_t clip_count() const noexcept { return clip_count_.load(std::memory_order_relaxed); }
  /// Number of notes currently sounding (audio-thread state; read for tests).
  size_t active_note_count() const noexcept { return active_count_; }
  uint32_t active_note_overflow_count() const noexcept {
    return active_note_overflow_count_.load(std::memory_order_relaxed);
  }
  uint32_t retained_channel_overflow_count() const noexcept {
    return retained_channel_overflow_count_.load(std::memory_order_relaxed);
  }
  uint32_t dispatched_event_count() const noexcept {
    return dispatched_event_count_.load(std::memory_order_relaxed);
  }
  uint32_t midi_fx_pending_overflow_count() const noexcept {
    return midi_fx_pending_overflow_count_.load(std::memory_order_relaxed);
  }

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
  struct RetainedChannelState {
    uint32_t destination_id = 0;
    uint8_t group = 0;
    uint8_t channel = 0;
    bool active = false;
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
  // sounding on a different destination/instrument. A fallback match never
  // crosses source tracks.
  void track_note_off(uint8_t group, uint8_t channel, uint8_t note, uint32_t destination_id,
                      uint32_t source_track_id, bool from_clip, uint32_t clip_id) noexcept;
  // Retains a channel triple for later controller-reset emission. `inserted`
  // identifies a new slot so a rejected note-on can roll that slot back when
  // the active-note table is full. Returns false only when a new slot cannot
  // be retained; existing entries always succeed.
  bool retain_channel_state(uint32_t destination_id, uint8_t group, uint8_t channel,
                            bool* inserted) noexcept;
  void release_retained_channel_state(uint32_t destination_id, uint8_t group, uint8_t channel,
                                      bool inserted) noexcept;
  void clear_retained_channel_states(bool single_destination, uint32_t destination_id) noexcept;
  void dispatch(uint32_t destination_id, const MidiEvent& event) noexcept;
  // Emit the standard reset sequence (damper off, reset-all-controllers,
  // all-notes-off, pitch-bend center) for one channel at render_frame. Used on a
  // playback discontinuity so a note released under a held sustain pedal does not
  // keep ringing and stale pitch-bend / CC state does not carry across.
  void emit_controller_reset(uint32_t destination_id, uint8_t group, uint8_t channel,
                             DeviceFrame render_frame) noexcept;
  // Emit emit_controller_reset() once per retained (destination, group, channel),
  // optionally limited to one destination. Non-mutating; RT-safe, no alloc.
  void emit_active_controller_resets(bool single_destination, uint32_t destination_id,
                                     DeviceFrame render_frame) noexcept;
  DestinationFx* find_midi_fx(uint32_t destination_id) noexcept;
  const DestinationFx* find_midi_fx(uint32_t destination_id) const noexcept;
  // Runs one device-framed event through the destination's MIDI-FX chain; output
  // later than the event waits in pending_fx.
  void process_event(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                     uint32_t clip_id) noexcept;
  // Visits every clip event and clip/iteration end on the clip timeline in
  // [from, last], in clip order, with its timeline frame.
  template <typename Visitor>
  void visit_scheduled(const std::vector<MidiClipSchedule>& clips, int64_t from, int64_t last,
                       Visitor&& visitor) const noexcept;
  void dispatch_transformed(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                            uint32_t clip_id) noexcept;
  // False when the pending list is full and the event was not kept.
  bool enqueue_pending(uint32_t destination_id, const MidiEvent& event, bool from_clip,
                       uint32_t clip_id) noexcept;
  // Remove one pending FX event without changing the order of the remaining
  // fixed-capacity queue. The audio path uses this instead of swap-remove so
  // same-frame controller gestures retain their stream order.
  void erase_pending(size_t index) noexcept;
  void clear_active_notes_for_channel(uint32_t destination_id, uint8_t group,
                                      uint8_t channel) noexcept;
  void clear_pending_note_events_for_channel(uint32_t destination_id, uint8_t group,
                                             uint8_t channel) noexcept;
  void clear_note_tracking_for_event(uint32_t destination_id, const MidiEvent& event,
                                     bool from_clip = false, uint32_t clip_id = 0) noexcept;
  void clear_pending_note_tracking_for_event(const PendingFxEvent& pending) noexcept;
  void retire_channel_mode_reset(uint32_t destination_id, uint8_t group, uint8_t channel) noexcept;
  void clear_pending_for_destination(uint32_t destination_id) noexcept;
  void clear_pending_for_clip(uint32_t clip_id) noexcept;
  void release_notes_for_clip(uint32_t clip_id, DeviceFrame render_frame,
                              bool clear_pending = true) noexcept;
  // Release note-offs for every sounding note (and drop pending FX events) whose
  // (clip id, destination, source track) is no longer present in `clips`
  // (nullptr = empty set). Called once when the published clip set changes so a
  // live mute / clip delete that recompiles and republishes without a clip does
  // not hang its notes.
  void release_notes_for_absent_clips(const std::vector<MidiClipSchedule>* clips,
                                      const SequencerClock& clock) noexcept;

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
  // Fixed-capacity channel-state table (audio thread only). It survives
  // natural/clip note-offs and is cleared only by reset or a global/destination
  // all-notes-off operation.
  std::array<RetainedChannelState, kMaxRetainedChannelStates> retained_channels_{};
  std::atomic<uint32_t> retained_channel_overflow_count_{0};
  std::atomic<uint32_t> dispatched_event_count_{0};
  mutable rt::RtPublisher<MidiFxSnapshot> midi_fx_snapshots_;
  const MidiFxSnapshot* last_midi_fx_snapshot_ = nullptr;
  // Audio-thread-only: the clock of the last dispatch_due, whose device frame of
  // timeline zero anchors a quantize grid for live injections.
  SequencerClock clock_{};
  uint64_t next_midi_fx_generation_ = 1;  // control-thread only
  std::unique_ptr<RuntimeStorage> runtime_storage_;
  size_t pending_fx_count_ = 0;
  std::atomic<uint32_t> midi_fx_pending_overflow_count_{0};
};

/// Whether @p ump ends sound or undoes held state: a note-off, or one of the
/// channel-reset messages a stop or panic sends (damper up, Reset All
/// Controllers, All Sound Off, All Notes Off, pitch bend at centre).
bool is_release_message(const Ump& ump) noexcept;

/// Release messages a destination refused (a full output queue), held until it
/// accepts them, so congestion delays a release instead of losing it. One entry
/// per (route, destination, message): a repeated refusal keeps one entry, and
/// the producers bound the distinct keys -- one note-off per sounding note and
/// four reset messages per retained channel -- which the capacity covers.
class ReleaseSet {
 public:
  static constexpr size_t kCapacity =
      MidiSequencer::kMaxActiveNotes + 4 * MidiSequencer::kMaxRetainedChannelStates;

  /// Records @p event for @p destination_id on @p route; a duplicate is ignored.
  void add(uint8_t route, uint32_t destination_id, const MidiEvent& event) noexcept;
  /// Offers every held entry, oldest first, restamped at @p render_frame, to
  /// @p deliver(route, destination_id, event), and keeps the ones it refuses.
  template <typename Deliver>
  void retry(int64_t render_frame, Deliver&& deliver) noexcept {
    size_t kept = 0;
    for (size_t i = 0; i < size_; ++i) {
      Entry entry = entries_[i];
      entry.event.render_frame = render_frame;
      if (!deliver(entry.route, entry.destination_id, entry.event)) entries_[kept++] = entry;
    }
    size_ = kept;
  }
  size_t size() const noexcept { return size_; }
  void clear() noexcept { size_ = 0; }

 private:
  struct Entry {
    uint8_t route = 0;
    uint32_t destination_id = 0;
    MidiEvent event{};
  };
  std::array<Entry, kCapacity> entries_{};
  size_t size_ = 0;
};

}  // namespace sonare::midi
