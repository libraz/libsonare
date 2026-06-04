#pragma once

/// @file midi_clip.h
/// @brief PPQ-timed MIDI clip (control-thread) + the RT-facing MidiClipSchedule
///        compiled from it.
///
/// Layering: this header depends ONLY on midi/ump, midi/midi_event and
/// transport/. It does NOT depend on arrangement/ or engine/ (those depend on
/// midi, never the reverse), so it can be linked into sonare_engine without a
/// dependency cycle.

#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

namespace sonare::midi {

/// One MIDI event positioned in musical time (PPQ, quarter-note units, matching
/// transport::TempoMap). Control-thread value data.
struct MidiClipEvent {
  /// Position within the clip's source timeline (PPQ, quarter notes).
  double ppq = 0.0;
  Ump ump{};
  /// Optional control-thread-resolved payload view for SysEx-handle UMPs.
  const uint8_t* sysex_payload = nullptr;
  size_t sysex_payload_size = 0;

  bool operator==(const MidiClipEvent& o) const noexcept {
    return ppq == o.ppq && ump == o.ump && sysex_payload == o.sysex_payload &&
           sysex_payload_size == o.sysex_payload_size;
  }
};

/// Result of validating a clip's note pairing.
struct NotePairValidation {
  /// True when every note-on has a matching note-off (and vice versa).
  bool ok = true;
  /// Count of note-ons that never received a matching note-off.
  uint32_t unmatched_note_ons = 0;
  /// Count of note-offs with no preceding matching note-on.
  uint32_t unmatched_note_offs = 0;
};

/// A PPQ-timed MIDI event list. Control-thread only; never read by RT directly
/// (the compiler bakes it into a MidiClipSchedule).
class MidiClip {
 public:
  MidiClip() = default;

  void set_events(std::vector<MidiClipEvent> events);
  void add_event(const MidiClipEvent& event);
  const std::vector<MidiClipEvent>& events() const noexcept { return events_; }

  /// Stable sort by (ppq, then note-off before note-on at the SAME ppq, then a
  /// stable tiebreak on note/channel/word so identical timestamps are ordered
  /// deterministically). Idempotent. No clock/random.
  void sort_stable();

  /// Validates that every note-on has a matching note-off (per
  /// group+channel+note, FIFO matching). Reports unmatched counts; does not
  /// mutate the clip.
  NotePairValidation validate_note_pairs() const;

  /// Converts the (sorted) PPQ events to absolute render-frame MidiEvents via
  /// `tempo_map`, offsetting each by `clip_start_ppq` so a clip placed at a
  /// musical position fires at the right sample. Appends to `out`.
  void to_render_events(const transport::TempoMap& tempo_map, double clip_start_ppq,
                        std::vector<MidiEvent>* out) const;

 private:
  std::vector<MidiClipEvent> events_;
};

/// Loop policy for a scheduled MIDI clip.
enum class MidiLoopMode : uint8_t {
  kOneShot = 0,
  kLoop = 1,
};

/// The RT-facing compiled MIDI clip. Owned value data handed to the engine via
/// the RtPublisher path (like engine::ClipSchedule). Events are already in
/// absolute render frames (PPQ->frame baked by the compiler). The RT sequencer
/// scans `events` for the current block and dispatches them.
struct MidiClipSchedule {
  /// Stable clip id (mirrors the EditClip id).
  uint32_t id = 0;
  /// Source-track id (mirrors the EditClip's track_id). Control-plane only; the
  /// sequencer never reads it. The offline bounce groups clips into per-track
  /// stems for channel-strip mixing. 0 = unset.
  uint32_t track_id = 0;
  /// Clip start on the render timeline (samples), baked from PPQ.
  int64_t start_sample = 0;
  /// Clip start in musical time (PPQ); carried for diagnostics / re-baking.
  double start_ppq = 0.0;
  /// Clip length on the render timeline (samples). 0 = open-ended.
  int64_t length_samples = 0;
  /// Loop policy.
  MidiLoopMode loop_mode = MidiLoopMode::kOneShot;
  /// Loop length in samples (used when loop_mode == kLoop and > 0).
  int64_t loop_length_samples = 0;
  /// Destination / route id the sequencer dispatches this clip's events to. 0 =
  /// default/null destination.
  uint32_t destination_id = 0;
  /// Absolute render-frame events, sorted ascending by render_frame. Note-off
  /// before note-on at the same frame (inherited from MidiClip::sort_stable).
  std::vector<MidiEvent> events;

  bool operator==(const MidiClipSchedule& o) const noexcept {
    return id == o.id && start_sample == o.start_sample && start_ppq == o.start_ppq &&
           length_samples == o.length_samples && loop_mode == o.loop_mode &&
           loop_length_samples == o.loop_length_samples && destination_id == o.destination_id &&
           events == o.events;
  }
};

}  // namespace sonare::midi
