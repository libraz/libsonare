#pragma once

/// @file prepared_sysex.h
/// @brief Control-thread prepared state retained by a scheduled SysEx event.

namespace sonare::midi {

/// Opaque, instrument-owned state prepared from a SysEx payload on the control
/// thread. The audio path only carries a pointer to this immutable operation,
/// owned by the published schedule's MidiSysExPayloadBank, or for live SysEx by
/// the engine's payload slot.
class PreparedMidiSysEx {
 public:
  virtual ~PreparedMidiSysEx() = default;
};

}  // namespace sonare::midi
