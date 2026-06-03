#pragma once

/// @file midi_event.h
/// @brief A render-frame-timestamped UMP event.

#include <cstdint>

#include "midi/ump.h"

namespace sonare::midi {

/// A single MIDI event placed on the render (sample) timeline. `render_frame` is
/// the absolute sample position at which the event fires; `ump` is the fixed POD
/// payload. Trivially copyable so it can ride RT structures.
struct MidiEvent {
  int64_t render_frame = 0;
  Ump ump{};

  bool operator==(const MidiEvent& o) const noexcept {
    return render_frame == o.render_frame && ump == o.ump;
  }
  bool operator!=(const MidiEvent& o) const noexcept { return !(*this == o); }
};

}  // namespace sonare::midi
