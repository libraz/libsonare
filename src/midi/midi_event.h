#pragma once

/// @file midi_event.h
/// @brief A render-frame-timestamped UMP event.

#include <cstddef>
#include <cstdint>

#include "midi/ump.h"

namespace sonare::midi {

/// A single MIDI event placed on the render (sample) timeline. `render_frame` is
/// the absolute sample position at which the event fires; `ump` is the fixed POD
/// payload. Trivially copyable so it can ride RT structures.
struct MidiEvent {
  int64_t render_frame = 0;
  Ump ump{};
  /// Optional control-thread-resolved SysEx payload view for UMPs that carry a
  /// sysex_handle. The pointed bytes must outlive the RT schedule using this
  /// event. RT code only copies this view; it never dereferences a SysExStore.
  const uint8_t* sysex_payload = nullptr;
  size_t sysex_payload_size = 0;

  bool operator==(const MidiEvent& o) const noexcept {
    return render_frame == o.render_frame && ump == o.ump && sysex_payload == o.sysex_payload &&
           sysex_payload_size == o.sysex_payload_size;
  }
  bool operator!=(const MidiEvent& o) const noexcept { return !(*this == o); }
};

}  // namespace sonare::midi
