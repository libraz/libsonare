#pragma once

/// @file instrument.h
/// @brief Host-instrument audio seam: an interface that is BOTH an
///        rt::ProcessorBase (audio render) and a midi::MidiEventSink (event
///        feed), so the engine can sum a MIDI-driven instrument's audio at the
///        clip/source-merge stage.
///
/// RT contract: prepare() runs on the control thread and is the only place
/// allocation is allowed; on_event() and process() run on the audio thread and
/// must be allocation-, lock- and I/O-free. latency_samples() feeds the
/// arrangement compiler's PDC summary.
///
/// EVENT CLOCK DOMAIN. MidiEvent::render_frame is always a DEVICE render frame —
/// the engine's free-running output-sample counter, monotonic across seek, loop
/// and stop — and every feed path (compiled clips, live input, queued commands,
/// hang-note releases) arrives in that one basis. The block's first frame is
/// TransportState::render_frame, so
///
///     offset = event.render_frame - state.render_frame
///
/// is the intra-block offset. Two traps: a block's events are delivered BEFORE
/// its set_transport(), so placement belongs in process() and not in on_event();
/// and an instrument-accumulated counter is not a substitute, because the engine
/// renders an instrument only while the transport rolls or a note is sounding,
/// so such a counter drifts the first time playback stops.
/// TransportState::sample_position is the timeline coordinate and must not place
/// events. One case escapes the [0, num_samples) guarantee: events queued while
/// the transport is stopped and nothing is sounding arrive with a negative
/// offset, so clamp to 0 rather than indexing out of the block.

#include <cstddef>
#include <cstdint>
#include <string>

#include "midi/articulation_mode.h"
#include "midi/controller_profile.h"
#include "midi/sequencer.h"
#include "rt/processor_base.h"
#include "transport/transport_state.h"

namespace sonare::midi {

/// One caller-owned render target for source-track-aware instruments. A target
/// with source_track_id == 0 is the deterministic fallback for direct/live
/// events and scheduled tracks that do not have a configured lane. The engine
/// clears every target before calling process_source_tracks().
struct MidiInstrumentSourceOutput {
  uint32_t source_track_id = 0;
  float* const* channels = nullptr;
};

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
  /// loop region). Its render_frame is the block's first DEVICE frame and is the
  /// basis every queued event must be placed against (see "Event clock domain"
  /// above), so an instrument that schedules events overrides this even when it
  /// ignores tempo. Must be allocation-free and lock-free. Default: ignored
  /// (a free-running instrument needs no transport).
  virtual void set_transport(const transport::TransportState& state) noexcept { (void)state; }

  /// CONTROL thread: apply a SysEx whose realised effect (e.g. a GS insertion-
  /// effect chain) must be built off the audio thread and handed over wait-free,
  /// so a live engine can hear it without stopping. Default: no-op — the
  /// audio-visible channel/EFX state is still delivered separately via on_event.
  /// Instruments that realise control-thread state (Sf2Player) override this.
  virtual void on_control_sysex(const uint8_t* data, size_t size) noexcept {
    (void)data;
    (void)size;
  }

  /// CONTROL thread: adopts a device's spelling of the expression axes, so a
  /// breath gesture reaches an exciter by what it means rather than by the
  /// controller number that carried it. Returns false when the instrument has
  /// nowhere to put one, which is the answer a caller needs: a silent success
  /// here is indistinguishable from a profile that took, and the next note
  /// would be the only evidence. Default: refused.
  virtual bool set_controller_profile(const ControllerProfile& profile) noexcept {
    (void)profile;
    return false;
  }

  /// CONTROL thread: the profile in force, or nullptr for an instrument that
  /// refuses one. Paired with the setter so a host can show the mapping it
  /// installed rather than the one it believes it installed.
  virtual const ControllerProfile* controller_profile() const noexcept { return nullptr; }

  /// CONTROL thread: how @p channel treats a note-on while another note on that
  /// channel is still held. Returns false when the instrument has no
  /// articulation of its own, on the same terms as set_controller_profile: a
  /// silent success is indistinguishable from a mode that took, and the next
  /// overlapping pair of notes would be the only evidence. Default: refused.
  ///
  /// Not reachable from a MIDI stream. CC126 names a monophonic mode but not
  /// kMonoLegato, and reading it as that one would change what a spec-compliant
  /// file sounds like.
  virtual bool set_articulation(uint8_t channel, ArticulationMode mode) noexcept {
    (void)channel;
    (void)mode;
    return false;
  }

  /// CONTROL thread: reads back set_articulation. Paired with it so a host can
  /// show the mode in force rather than the one it believes it installed.
  virtual bool articulation(uint8_t channel, ArticulationMode* out) const noexcept {
    (void)channel;
    (void)out;
    return false;
  }

  /// CONTROL thread: how many times a legato continuation was asked for and
  /// refused, so the note started a voice of its own instead. Counted rather
  /// than inferred: a refusal sounds like an ordinary note, so nothing in the
  /// audio separates "this engine declines legato" from "the mode was never
  /// set".
  ///
  /// Refused by the same instruments that refuse the two calls above, rather
  /// than answering zero: an instrument that never had an articulation has
  /// refused nothing, and a host reading that zero would read it as "every slur
  /// took" — which is the reading this counter exists to prevent. Default:
  /// refused.
  virtual bool legato_fallback_count(uint64_t* out) const noexcept {
    (void)out;
    return false;
  }

  /// AUDIO thread: render one block into source-track-specific output targets.
  /// Implementations must advance every voice and shared DSP state exactly once,
  /// then add each attributable contribution to its matching target. Events for
  /// a track absent from @p outputs go to its source_track_id == 0 fallback.
  ///
  /// Returning false requests the legacy destination-level process() path. This
  /// preserves compatibility for opaque host callback instruments which cannot
  /// expose per-voice provenance. Builtin and native instruments override this;
  /// callers must still accept false without allocating on the audio thread.
  virtual bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                                     int num_channels, int num_samples) noexcept {
    (void)outputs;
    (void)output_count;
    (void)num_channels;
    (void)num_samples;
    return false;
  }

  /// True when process_source_tracks() is implemented without changing this
  /// instrument's destination-level voice-pool semantics.
  virtual bool supports_source_track_rendering() const noexcept { return false; }

  /// CONTROL thread: resolves a JSON-key parameter name to this instrument's
  /// integer param id, or -1 when the key names no continuously automatable
  /// parameter. Mirrors mixing::ChannelStrip::insert_parameter_id_for_key, so a
  /// host resolves a name once and then drives the id from an automation lane.
  ///
  /// Only parameters that are meaningful to change WHILE the instrument sounds
  /// belong here. Structural parameters (engine mode, oscillator waveform,
  /// unison width, voice-pool size) must return -1: they are patch edits, not
  /// automation targets. Default: no automatable parameters.
  virtual int parameter_id_for_key(const std::string& key) const noexcept {
    (void)key;
    return -1;
  }

  /// AUDIO thread: applies one resolved parameter. Called at most once per
  /// parameter per block by the engine's smoother bank, so the effective
  /// resolution is one audio block. Must be allocation-free, lock-free and
  /// I/O-free, and must clamp @p value to the parameter's audible range.
  /// Returns false for an unknown id. Default: nothing is automatable.
  virtual bool apply_parameter(unsigned int param_id, float value) noexcept {
    (void)param_id;
    (void)value;
    return false;
  }
};

}  // namespace sonare::midi
