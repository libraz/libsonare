#include "midi/channel_voice_decode.h"

namespace sonare::midi {

namespace {

bool decode_midi1(const Ump& ump, ChannelVoiceEvent* out) noexcept {
  const uint8_t status = ump.status_nibble();
  if (status < 0x8u) return false;
  const uint32_t w = ump.words[0];
  const auto d1 = static_cast<uint8_t>((w >> 8) & 0x7Fu);
  const auto d2 = static_cast<uint8_t>(w & 0x7Fu);

  ChannelVoiceEvent ev;
  switch (static_cast<UmpStatus>(status)) {
    case UmpStatus::kNoteOff:
      ev.kind = ChannelVoiceKind::NoteOff;
      ev.note = d1;
      ev.velocity = Velocity16::from7(d2);
      break;
    case UmpStatus::kNoteOn:
      ev.note = d1;
      if (d2 == 0) {
        ev.kind = ChannelVoiceKind::NoteOff;
        ev.velocity = Velocity16::from_raw(0x8000u);
      } else {
        ev.kind = ChannelVoiceKind::NoteOn;
        ev.velocity = Velocity16::from7(d2);
      }
      break;
    case UmpStatus::kPolyPressure:
      ev.kind = ChannelVoiceKind::PolyPressure;
      ev.note = d1;
      ev.value = Control32::from7(d2);
      break;
    case UmpStatus::kControlChange:
      ev.kind = ChannelVoiceKind::ControlChange;
      ev.note = d1;
      ev.value = Control32::from7(d2);
      break;
    case UmpStatus::kProgramChange:
      ev.kind = ChannelVoiceKind::ProgramChange;
      ev.program = d1;
      break;
    case UmpStatus::kChannelPressure:
      ev.kind = ChannelVoiceKind::ChannelPressure;
      ev.value = Control32::from7(d1);
      break;
    case UmpStatus::kPitchBend:
      ev.kind = ChannelVoiceKind::PitchBend;
      ev.bend = Bend32::from14(static_cast<uint16_t>((static_cast<uint16_t>(d2) << 7u) | d1));
      break;
    default:
      return false;
  }
  ev.group = ump_group_from_word0(w);
  ev.channel = ump.channel();
  *out = ev;
  return true;
}

bool decode_midi2(const Ump& ump, ChannelVoiceEvent* out) noexcept {
  const uint32_t w0 = ump.words[0];
  const uint32_t w1 = ump.words[1];
  const auto b3 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
  const auto b4 = static_cast<uint8_t>(w0 & 0xFFu);

  ChannelVoiceEvent ev;
  switch (ump.status_nibble()) {
    case 0x0:
    case 0x1:
      ev.kind = ump.status_nibble() == 0x0 ? ChannelVoiceKind::RegisteredPerNote
                                           : ChannelVoiceKind::AssignablePerNote;
      ev.note = b3 & 0x7Fu;
      ev.index = b4;
      ev.value = Control32::from_raw(w1);
      break;
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x5:
      switch (ump.status_nibble()) {
        case 0x2:
          ev.kind = ChannelVoiceKind::RegisteredController;
          break;
        case 0x3:
          ev.kind = ChannelVoiceKind::AssignableController;
          break;
        case 0x4:
          ev.kind = ChannelVoiceKind::RelativeRegistered;
          break;
        default:
          ev.kind = ChannelVoiceKind::RelativeAssignable;
          break;
      }
      ev.bank = b3 & 0x7Fu;
      ev.index = b4 & 0x7Fu;
      ev.value = Control32::from_raw(w1);
      break;
    case 0x6:
      ev.kind = ChannelVoiceKind::PerNotePitchBend;
      ev.note = b3 & 0x7Fu;
      ev.bend = Bend32::from_raw(w1);
      break;
    case 0x8:
    case 0x9:
      ev.kind = ump.status_nibble() == 0x8 ? ChannelVoiceKind::NoteOff : ChannelVoiceKind::NoteOn;
      ev.note = b3 & 0x7Fu;
      ev.index = b4;
      ev.velocity = Velocity16::from_raw(static_cast<uint16_t>(w1 >> 16));
      ev.attribute_data = static_cast<uint16_t>(w1 & 0xFFFFu);
      break;
    case 0xA:
      ev.kind = ChannelVoiceKind::PolyPressure;
      ev.note = b3 & 0x7Fu;
      ev.value = Control32::from_raw(w1);
      break;
    case 0xB:
      ev.kind = ChannelVoiceKind::ControlChange;
      ev.note = b3 & 0x7Fu;
      ev.value = Control32::from_raw(w1);
      break;
    case 0xC:
      ev.kind = ChannelVoiceKind::ProgramChange;
      ev.flags = b4 & 0x01u;
      ev.program = static_cast<uint8_t>((w1 >> 24) & 0x7Fu);
      ev.bank_msb = static_cast<uint8_t>((w1 >> 8) & 0x7Fu);
      ev.bank_lsb = static_cast<uint8_t>(w1 & 0x7Fu);
      break;
    case 0xD:
      ev.kind = ChannelVoiceKind::ChannelPressure;
      ev.value = Control32::from_raw(w1);
      break;
    case 0xE:
      ev.kind = ChannelVoiceKind::PitchBend;
      ev.bend = Bend32::from_raw(w1);
      break;
    case 0xF:
      ev.kind = ChannelVoiceKind::PerNoteManagement;
      ev.note = b3 & 0x7Fu;
      ev.flags = b4 & 0x03u;
      break;
    default:
      return false;
  }
  ev.group = ump_group_from_word0(w0);
  ev.channel = ump.channel();
  *out = ev;
  return true;
}

}  // namespace

bool decode_channel_voice(const Ump& ump, ChannelVoiceEvent* out) noexcept {
  switch (ump.message_type()) {
    case UmpMessageType::kMidi1ChannelVoice:
      return decode_midi1(ump, out);
    case UmpMessageType::kMidi2ChannelVoice:
      return decode_midi2(ump, out);
    default:
      return false;
  }
}

}  // namespace sonare::midi
