#pragma once

/// @file midi_event.h
/// @brief A render-frame-timestamped UMP event.

#include <cstddef>
#include <cstdint>

#include "midi/ump.h"

namespace sonare::midi {

class PreparedMidiSysEx;

/// A single MIDI event placed on the render (sample) timeline. `render_frame` is
/// the absolute sample position at which the event fires; `ump` is the fixed POD
/// payload. Trivially copyable so it can ride RT structures.
struct MidiEvent {
  int64_t render_frame = 0;
  Ump ump{};
  /// Source track of a scheduled event. 0 identifies a direct/live event with
  /// no arrangement-track owner. This survives MIDI-FX expansion and synthetic
  /// clip-end releases so a shared instrument can later render voices into the
  /// correct track lane without changing its destination/voice-pool identity.
  uint32_t source_track_id = 0;
  /// Optional control-thread-resolved SysEx payload view for UMPs that carry a
  /// sysex_handle. A published schedule's payload bank owns the bytes; other
  /// event paths borrow them from their caller. RT code only copies this view;
  /// the sink consumes it synchronously and never dereferences a SysExStore.
  const uint8_t* sysex_payload = nullptr;
  size_t sysex_payload_size = 0;
  /// Optional control-thread-prepared operation for this SysEx event, owned by
  /// the published schedule's MidiSysExPayloadBank, or for live SysEx by the
  /// engine's payload slot. Only the destination instrument dereferences it,
  /// during synchronous dispatch.
  const PreparedMidiSysEx* prepared_sysex = nullptr;

  bool operator==(const MidiEvent& o) const noexcept {
    return render_frame == o.render_frame && ump == o.ump && source_track_id == o.source_track_id &&
           sysex_payload == o.sysex_payload && sysex_payload_size == o.sysex_payload_size &&
           prepared_sysex == o.prepared_sysex;
  }
  bool operator!=(const MidiEvent& o) const noexcept { return !(*this == o); }
};

/// Whether @p event is a SysEx operation: a UMP carrying a SysEx-store handle, or
/// a Data (MT 0x3 SysEx7 / MT 0x5 128-bit) message.
inline bool is_sysex_event(const MidiEvent& event) noexcept {
  const UmpMessageType type = event.ump.message_type();
  return event.ump.sysex_handle != 0 || type == UmpMessageType::kData64 ||
         type == UmpMessageType::kData128;
}

}  // namespace sonare::midi
