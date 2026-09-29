#pragma once

/// @file channel_voice_decode.h
/// @brief Decodes one channel-voice UMP (MIDI 1.0 or MIDI 2.0 protocol) into a single event
///        whose values are held at MIDI 2.0 width.
///
/// MIDI 1.0 values are widened on receipt (see control_value.h); MIDI 2.0 raw bits are carried
/// unchanged. The decoder is stateless and real-time safe: no allocation, no exceptions.
/// Bank-select, RPN and NRPN sequences are not tracked here; MIDI 1.0 CC 0/32/6/38/98-101 come out
/// as plain ControlChange events.

#include <cstdint>

#include "midi/control_value.h"
#include "midi/ump.h"

namespace sonare::midi {

/// Kind of a decoded channel-voice event.
enum class ChannelVoiceKind : uint8_t {
  NoteOn,
  NoteOff,
  PolyPressure,
  ControlChange,
  ProgramChange,
  ChannelPressure,
  PitchBend,
  RegisteredController,
  AssignableController,
  RelativeRegistered,
  RelativeAssignable,
  RegisteredPerNote,
  AssignablePerNote,
  PerNotePitchBend,
  PerNoteManagement,
};

/// One decoded channel-voice event. Fields not used by a kind are zero.
struct ChannelVoiceEvent {
  ChannelVoiceKind kind = ChannelVoiceKind::NoteOn;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;        ///< Note number, or the CC index for ControlChange.
  uint8_t index = 0;       ///< RPNC/APNC index, RC/AC index, or note attribute type.
  uint8_t bank = 0;        ///< RC/AC bank.
  uint8_t flags = 0;       ///< ProgramChange bank-valid = bit0; PerNoteManagement reset S = bit0,
                           ///< detach D = bit1.
  Velocity16 velocity{0};  ///< NoteOn / NoteOff, raw as received.
  uint16_t attribute_data = 0;
  Control32 value{0};  ///< CC, pressures, RC/AC/RPNC/APNC data; relative kinds hold a two's
                       ///< complement delta in raw.
  Bend32 bend{0};      ///< PitchBend, PerNotePitchBend.
  uint8_t program = 0;
  uint8_t bank_msb = 0;
  uint8_t bank_lsb = 0;
};

/// Decodes a MIDI 1.0 (MT 0x2) or MIDI 2.0 (MT 0x4) channel-voice message.
///
/// Returns false, leaving @p out untouched, for a reserved MIDI 2.0 status (0x7) and for a MIDI 1.0
/// status nibble below 0x8, and for any other message type. The caller tells "not channel voice"
/// (check Ump::message_type() first) apart from "reserved status" (count it as skipped).
///
/// A MIDI 1.0 Note On with velocity 0 decodes as NoteOff with velocity raw 0x8000. A MIDI 2.0 Note
/// On with velocity 0 stays NoteOn with raw 0 preserved.
bool decode_channel_voice(const Ump& ump, ChannelVoiceEvent* out) noexcept;

/// True for the registered controllers a synth reads straight from the decoded event: 0/0 (pitch
/// bend sensitivity), 0/1 (fine tuning), 0/2 (coarse tuning) and 0/7 (per-note pitch bend
/// sensitivity). Every other registered/assignable controller is lowered by the caller through
/// midi2_to_midi1_messages; lowering is not the decoder's job.
inline bool is_directly_held_registered_controller(uint8_t bank, uint8_t index) {
  return bank == 0 && (index == 0 || index == 1 || index == 2 || index == 7);
}

}  // namespace sonare::midi
