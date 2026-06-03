#pragma once

/// @file instrument.h
/// @brief Host-instrument audio seam: an interface that is BOTH an
///        rt::ProcessorBase (audio render) and a midi::MidiEventSink (event
///        feed), so the engine can sum a MIDI-driven instrument's audio at the
///        clip/source-merge stage.
///
/// Scope: this is the seam, exercised now with a mock. Concrete host
/// instrument graph nodes (live plugins / SF2 / soft-synths) can be provided by the host; this
/// interface is what that node will implement.
///
/// Threading / RT contract
/// -----------------------
///  - prepare(sample_rate, max_block_size) runs on the CONTROL thread; it MAY
///    allocate (scratch / voice tables). It is the only place allocation is
///    allowed.
///  - on_event(...) and process(...) run on the AUDIO thread and MUST be
///    allocation-free, lock-free and I/O-free. on_event() is delivered by the
///    MidiSequencer at sample-accurate render frames during a block; process()
///    then renders the block's audio. The instrument is responsible for placing
///    each event at its intra-block sample offset (event.render_frame minus the
///    block's first render frame).
///  - latency_samples() (inherited from rt::ProcessorBase) reports the
///    instrument's reported latency in samples; the arrangement compiler folds
///    this into the CompiledTimeline PDC / latency summary.

#include "midi/sequencer.h"
#include "rt/processor_base.h"
#include "transport/transport_state.h"

namespace sonare::midi {

/// A host instrument node usable at the engine clip/source-merge stage.
///
/// IS-A rt::ProcessorBase  -> the engine renders it like any source/insert.
/// IS-A midi::MidiEventSink -> the MidiSequencer dispatches events to it.
///
/// A single instrument node is enough for the current engine wiring; the engine keeps one optional
/// instrument pointer (default nullptr / opt-in). When no instrument is set the
/// engine behaves exactly as before (no audio change, no event delivery).
class MidiInstrument : public rt::ProcessorBase, public MidiEventSink {
 public:
  ~MidiInstrument() override = default;

  // rt::ProcessorBase: prepare / process / reset / latency_samples are inherited
  // (prepare and process are pure-virtual and must be implemented).
  // MidiEventSink: on_event(destination_id, event) is inherited (pure-virtual).
  // rt::ProcessorBase::save_state / load_state provide opaque session
  // persistence (default: stateless).

  /// AUDIO thread: per-block playhead / transport sync, pushed by the engine
  /// BEFORE process() so a tempo-synced delay, arpeggiator or LFO inside a
  /// hosted instrument follows the host transport instead of free-running. The
  /// state is the same immutable per-block snapshot the engine feeds the
  /// sequencer / automation (playing, ppq/sample position, bpm, time signature,
  /// loop region). Must be allocation-free and lock-free. Default: ignored
  /// (a free-running instrument needs no transport).
  virtual void set_transport(const transport::TransportState& state) noexcept { (void)state; }
};

}  // namespace sonare::midi
