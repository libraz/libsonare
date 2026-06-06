#include "midi/ump.h"

#include <limits>

namespace sonare::midi {
namespace {

constexpr uint32_t kTypeShift = 28u;
constexpr uint32_t kStatusShift = 20u;
constexpr uint32_t kChannelShift = 16u;

uint32_t midi1_word0(uint8_t group, UmpStatus status, uint8_t channel, uint8_t data1,
                     uint8_t data2) noexcept {
  return (static_cast<uint32_t>(UmpMessageType::kMidi1ChannelVoice) << kTypeShift) |
         (static_cast<uint32_t>(group & 0x0Fu) << 24u) |
         (static_cast<uint32_t>(status) << kStatusShift) |
         (static_cast<uint32_t>(channel & 0x0Fu) << kChannelShift) |
         (static_cast<uint32_t>(data1 & 0x7Fu) << 8u) | static_cast<uint32_t>(data2 & 0x7Fu);
}

uint32_t midi2_word0(uint8_t group, uint8_t status_nibble, uint8_t channel, uint8_t byte2,
                     uint8_t byte3) noexcept {
  return (static_cast<uint32_t>(UmpMessageType::kMidi2ChannelVoice) << kTypeShift) |
         (static_cast<uint32_t>(group & 0x0Fu) << 24u) |
         (static_cast<uint32_t>(status_nibble & 0x0Fu) << kStatusShift) |
         (static_cast<uint32_t>(channel & 0x0Fu) << kChannelShift) |
         (static_cast<uint32_t>(byte2) << 8u) | static_cast<uint32_t>(byte3);
}

Ump make_midi1(uint8_t group, UmpStatus status, uint8_t channel, uint8_t data1,
               uint8_t data2) noexcept {
  Ump ump;
  ump.words[0] = midi1_word0(group, status, channel, data1, data2);
  ump.word_count = 1;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

}  // namespace

namespace {

// MIDI-Association "min-center-max" up-scaling (UMP spec, section on
// downscaling/upscaling). Maps a `src_bits`-wide value to `dst_bits`-wide:
// below center it is a left shift; at/above center it interpolates so the
// center maps to the destination center and the max maps to the destination
// max. The DOWN-scale is always a plain top-bit truncation (right shift), which
// guarantees a lossless src->dst->src round-trip of the top `src_bits` bits.
uint32_t scale_up(uint32_t value, uint32_t src_bits, uint32_t dst_bits) noexcept {
  const uint32_t scale_bits = dst_bits - src_bits;
  const uint32_t left = value << scale_bits;
  const uint32_t src_center = 1u << (src_bits - 1u);
  if (value <= src_center) {
    return left;
  }
  const uint32_t repeat_bits = src_bits - 1u;
  const uint32_t repeat_mask = (1u << repeat_bits) - 1u;
  const uint32_t bit_repeat = value & repeat_mask;
  uint32_t extra = bit_repeat;
  if (scale_bits >= repeat_bits) {
    extra <<= (scale_bits - repeat_bits);
  } else {
    extra >>= (repeat_bits - scale_bits);
  }
  return left | extra;
}

}  // namespace

uint16_t scale_velocity_7_to_16(uint8_t velocity7) noexcept {
  return static_cast<uint16_t>(scale_up(velocity7 & 0x7Fu, 7u, 16u));
}

uint8_t scale_velocity_16_to_7(uint16_t velocity16) noexcept {
  return static_cast<uint8_t>(velocity16 >> 9u);
}

uint8_t scale_note_on_velocity_16_to_7(uint16_t velocity16) noexcept {
  const uint8_t velocity7 = scale_velocity_16_to_7(velocity16);
  return velocity16 != 0 && velocity7 == 0 ? 1u : velocity7;
}

uint32_t scale_cc_7_to_32(uint8_t value7) noexcept { return scale_up(value7 & 0x7Fu, 7u, 32u); }

uint8_t scale_cc_32_to_7(uint32_t value32) noexcept { return static_cast<uint8_t>(value32 >> 25u); }

uint32_t scale_bend_14_to_32(uint16_t bend14) noexcept {
  return scale_up(bend14 & 0x3FFFu, 14u, 32u);
}

uint16_t scale_bend_32_to_14(uint32_t bend32) noexcept {
  return static_cast<uint16_t>(bend32 >> 18u);
}

Ump make_midi1_note_on(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity7) noexcept {
  return make_midi1(group, UmpStatus::kNoteOn, channel, note, velocity7);
}

Ump make_midi1_note_off(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity7) noexcept {
  return make_midi1(group, UmpStatus::kNoteOff, channel, note, velocity7);
}

Ump make_midi1_poly_pressure(uint8_t group, uint8_t channel, uint8_t note,
                             uint8_t pressure7) noexcept {
  return make_midi1(group, UmpStatus::kPolyPressure, channel, note, pressure7);
}

Ump make_midi1_control_change(uint8_t group, uint8_t channel, uint8_t controller,
                              uint8_t value7) noexcept {
  return make_midi1(group, UmpStatus::kControlChange, channel, controller, value7);
}

Ump make_midi1_program_change(uint8_t group, uint8_t channel, uint8_t program) noexcept {
  return make_midi1(group, UmpStatus::kProgramChange, channel, program, 0);
}

Ump make_midi1_channel_pressure(uint8_t group, uint8_t channel, uint8_t pressure7) noexcept {
  return make_midi1(group, UmpStatus::kChannelPressure, channel, pressure7, 0);
}

Ump make_midi1_pitch_bend(uint8_t group, uint8_t channel, uint16_t bend14) noexcept {
  const uint8_t lsb = static_cast<uint8_t>(bend14 & 0x7Fu);
  const uint8_t msb = static_cast<uint8_t>((bend14 >> 7u) & 0x7Fu);
  return make_midi1(group, UmpStatus::kPitchBend, channel, lsb, msb);
}

Ump make_midi2_note_on(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16,
                       uint8_t attribute_type, uint16_t attribute_data) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kNoteOn), channel,
                             static_cast<uint8_t>(note & 0x7Fu), attribute_type);
  ump.words[1] = (static_cast<uint32_t>(velocity16) << 16u) | attribute_data;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_note_off(uint8_t group, uint8_t channel, uint8_t note,
                        uint16_t velocity16) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kNoteOff), channel,
                             static_cast<uint8_t>(note & 0x7Fu), 0);
  ump.words[1] = static_cast<uint32_t>(velocity16) << 16u;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_poly_pressure(uint8_t group, uint8_t channel, uint8_t note,
                             uint32_t pressure32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kPolyPressure), channel,
                             static_cast<uint8_t>(note & 0x7Fu), 0);
  ump.words[1] = pressure32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_control_change(uint8_t group, uint8_t channel, uint8_t controller,
                              uint32_t value32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kControlChange), channel,
                             static_cast<uint8_t>(controller & 0x7Fu), 0);
  ump.words[1] = value32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_program_change(uint8_t group, uint8_t channel, uint8_t program, uint8_t bank_msb,
                              uint8_t bank_lsb, bool bank_valid) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kProgramChange), channel, 0,
                             bank_valid ? 0x01u : 0x00u);
  ump.words[1] = (static_cast<uint32_t>(program & 0x7Fu) << 24u) |
                 (static_cast<uint32_t>(bank_msb & 0x7Fu) << 8u) |
                 static_cast<uint32_t>(bank_lsb & 0x7Fu);
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_channel_pressure(uint8_t group, uint8_t channel, uint32_t pressure32) noexcept {
  Ump ump;
  ump.words[0] =
      midi2_word0(group, static_cast<uint8_t>(UmpStatus::kChannelPressure), channel, 0, 0);
  ump.words[1] = pressure32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_pitch_bend(uint8_t group, uint8_t channel, uint32_t bend32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kPitchBend), channel, 0, 0);
  ump.words[1] = bend32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_per_note_controller(uint8_t group, uint8_t channel, uint8_t note, uint8_t index,
                                   uint32_t value32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController),
                             channel, static_cast<uint8_t>(note & 0x7Fu), index);
  ump.words[1] = value32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_assignable_per_note_controller(uint8_t group, uint8_t channel, uint8_t note,
                                              uint8_t index, uint32_t value32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController),
                             channel, static_cast<uint8_t>(note & 0x7Fu), index);
  ump.words[1] = value32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_registered_controller(uint8_t group, uint8_t channel, uint8_t bank, uint8_t index,
                                     uint32_t value32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kRegisteredController), channel,
                             bank, index);
  ump.words[1] = value32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_midi2_assignable_controller(uint8_t group, uint8_t channel, uint8_t bank, uint8_t index,
                                     uint32_t value32) noexcept {
  Ump ump;
  ump.words[0] = midi2_word0(group, static_cast<uint8_t>(UmpStatus::kAssignableController), channel,
                             bank, index);
  ump.words[1] = value32;
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  return ump;
}

Ump make_sysex_handle(uint8_t group, SysExHandle handle) noexcept {
  Ump ump;
  ump.words[0] = (static_cast<uint32_t>(UmpMessageType::kData64) << kTypeShift) |
                 (static_cast<uint32_t>(group & 0x0Fu) << 24u);
  ump.word_count = 2;
  ump.group = static_cast<uint8_t>(group & 0x0Fu);
  ump.sysex_handle = handle;
  return ump;
}

SysExHandle SysExStore::allocate_handle() noexcept {
  if (payloads_.size() >= static_cast<size_t>(std::numeric_limits<SysExHandle>::max() - 1u)) {
    return 0;
  }
  for (uint64_t attempts = 0; attempts <= std::numeric_limits<SysExHandle>::max(); ++attempts) {
    const SysExHandle candidate = next_handle_++;
    if (next_handle_ == 0) next_handle_ = 1;
    if (candidate != 0 && payloads_.find(candidate) == payloads_.end()) {
      return candidate;
    }
  }
  return 0;
}

SysExHandle SysExStore::add(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  const SysExHandle handle = allocate_handle();
  if (handle == 0) {
    return 0;
  }
  payloads_[handle] = std::vector<uint8_t>(data, data + size);
  return handle;
}

const std::vector<uint8_t>* SysExStore::lookup(SysExHandle handle) const noexcept {
  if (handle == 0) {
    return nullptr;
  }
  const auto it = payloads_.find(handle);
  return it == payloads_.end() ? nullptr : &it->second;
}

bool SysExStore::remove(SysExHandle handle) noexcept {
  if (handle == 0) {
    return false;
  }
  return payloads_.erase(handle) != 0;
}

void SysExStore::clear() noexcept { payloads_.clear(); }

namespace {

int midi1_status_data_count(uint8_t status_nibble) noexcept {
  switch (static_cast<UmpStatus>(status_nibble)) {
    case UmpStatus::kProgramChange:
    case UmpStatus::kChannelPressure:
      return 1;
    case UmpStatus::kNoteOff:
    case UmpStatus::kNoteOn:
    case UmpStatus::kPolyPressure:
    case UmpStatus::kControlChange:
    case UmpStatus::kPitchBend:
      return 2;
    default:
      return -1;
  }
}

}  // namespace

size_t midi1_bytes_to_ump(const uint8_t* bytes, size_t len, uint8_t group, uint8_t* running_status,
                          Ump* out) noexcept {
  if (bytes == nullptr || out == nullptr || len == 0) {
    return 0;
  }
  size_t pos = 0;
  uint8_t status = bytes[0];
  size_t consumed_status = 0;
  if (status & 0x80u) {
    // System messages (0xF0..0xFF) are not channel-voice; do not handle here.
    if ((status & 0xF0u) == 0xF0u) {
      return 0;
    }
    if (running_status != nullptr) {
      *running_status = status;
    }
    pos = 1;
    consumed_status = 1;
  } else {
    // Running status: reuse the last status byte.
    if (running_status == nullptr || (*running_status & 0x80u) == 0) {
      return 0;
    }
    status = *running_status;
  }
  (void)consumed_status;

  const uint8_t status_nibble = static_cast<uint8_t>((status >> 4u) & 0x0Fu);
  const uint8_t channel = static_cast<uint8_t>(status & 0x0Fu);
  const int data_count = midi1_status_data_count(status_nibble);
  if (data_count < 0) {
    return 0;
  }
  if (len - pos < static_cast<size_t>(data_count)) {
    return 0;  // Incomplete message.
  }
  const uint8_t d1 = bytes[pos];
  const uint8_t d2 = data_count == 2 ? bytes[pos + 1] : 0;
  *out = make_midi1(group, static_cast<UmpStatus>(status_nibble), channel, d1, d2);
  return pos + static_cast<size_t>(data_count);
}

size_t ump_to_midi1_bytes(const Ump& ump, uint8_t* out, size_t cap) noexcept {
  if (out == nullptr || ump.message_type() != UmpMessageType::kMidi1ChannelVoice) {
    return 0;
  }
  const uint8_t status_nibble = ump.status_nibble();
  const int data_count = midi1_status_data_count(status_nibble);
  if (data_count < 0) {
    return 0;
  }
  const size_t total = 1u + static_cast<size_t>(data_count);
  if (cap < total) {
    return 0;
  }
  out[0] = static_cast<uint8_t>((status_nibble << 4u) | ump.channel());
  out[1] = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
  if (data_count == 2) {
    out[2] = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
  }
  return total;
}

Ump midi1_to_midi2(const Ump& ump) noexcept {
  if (ump.message_type() != UmpMessageType::kMidi1ChannelVoice) {
    return ump;
  }
  const uint8_t status = ump.status_nibble();
  const uint8_t channel = ump.channel();
  const uint8_t d1 = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
  const uint8_t d2 = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
  switch (static_cast<UmpStatus>(status)) {
    case UmpStatus::kNoteOn:
      return make_midi2_note_on(ump.group, channel, d1, scale_velocity_7_to_16(d2));
    case UmpStatus::kNoteOff:
      return make_midi2_note_off(ump.group, channel, d1, scale_velocity_7_to_16(d2));
    case UmpStatus::kPolyPressure:
      return make_midi2_poly_pressure(ump.group, channel, d1, scale_cc_7_to_32(d2));
    case UmpStatus::kControlChange:
      return make_midi2_control_change(ump.group, channel, d1, scale_cc_7_to_32(d2));
    case UmpStatus::kProgramChange:
      return make_midi2_program_change(ump.group, channel, d1, 0, 0, false);
    case UmpStatus::kChannelPressure:
      return make_midi2_channel_pressure(ump.group, channel, scale_cc_7_to_32(d1));
    case UmpStatus::kPitchBend: {
      const uint16_t bend14 = static_cast<uint16_t>((static_cast<uint16_t>(d2) << 7u) | d1);
      return make_midi2_pitch_bend(ump.group, channel, scale_bend_14_to_32(bend14));
    }
    default:
      return ump;
  }
}

Ump midi2_to_midi1(const Ump& ump) noexcept {
  if (ump.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return ump;
  }
  const uint8_t status = ump.status_nibble();
  const uint8_t channel = ump.channel();
  const uint8_t note = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
  switch (static_cast<UmpStatus>(status)) {
    case UmpStatus::kNoteOn: {
      const uint16_t velocity16 = static_cast<uint16_t>(ump.words[1] >> 16u);
      return make_midi1_note_on(ump.group, channel, note,
                                scale_note_on_velocity_16_to_7(velocity16));
    }
    case UmpStatus::kNoteOff: {
      const uint16_t velocity16 = static_cast<uint16_t>(ump.words[1] >> 16u);
      return make_midi1_note_off(ump.group, channel, note, scale_velocity_16_to_7(velocity16));
    }
    case UmpStatus::kControlChange:
      return make_midi1_control_change(ump.group, channel, note, scale_cc_32_to_7(ump.words[1]));
    case UmpStatus::kProgramChange:
      return make_midi1_program_change(ump.group, channel,
                                       static_cast<uint8_t>((ump.words[1] >> 24u) & 0x7Fu));
    case UmpStatus::kPolyPressure:
      return make_midi1_poly_pressure(ump.group, channel, note, scale_cc_32_to_7(ump.words[1]));
    case UmpStatus::kChannelPressure:
      return make_midi1_channel_pressure(ump.group, channel, scale_cc_32_to_7(ump.words[1]));
    case UmpStatus::kPitchBend:
      return make_midi1_pitch_bend(ump.group, channel, scale_bend_32_to_14(ump.words[1]));
    case UmpStatus::kRegisteredPerNoteController:
    case UmpStatus::kAssignablePerNoteController:
    case UmpStatus::kRegisteredController:
    case UmpStatus::kAssignableController: {
      // MIDI 2.0 controller forms have no MIDI 1.0 equivalent: signal "drop me".
      Ump dropped;
      dropped.word_count = 0;
      return dropped;
    }
    default:
      return ump;
  }
}

Midi1MessageList midi2_to_midi1_messages(const Ump& ump) noexcept {
  Midi1MessageList out;
  if (ump.message_type() == UmpMessageType::kMidi1ChannelVoice && ump.word_count != 0) {
    out.messages[0] = ump;
    out.count = 1;
    return out;
  }
  if (ump.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return out;
  }

  const uint8_t status = ump.status_nibble();
  const uint8_t channel = ump.channel();
  if (status == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    const bool bank_valid = (ump.words[0] & 0x01u) != 0;
    const uint8_t program = static_cast<uint8_t>((ump.words[1] >> 24u) & 0x7Fu);
    if (bank_valid) {
      const uint8_t bank_msb = static_cast<uint8_t>((ump.words[1] >> 8u) & 0x7Fu);
      const uint8_t bank_lsb = static_cast<uint8_t>(ump.words[1] & 0x7Fu);
      out.messages[0] = make_midi1_control_change(ump.group, channel, 0, bank_msb);
      out.messages[1] = make_midi1_control_change(ump.group, channel, 32, bank_lsb);
      out.messages[2] = make_midi1_program_change(ump.group, channel, program);
      out.count = 3;
      return out;
    }
  }

  const Ump single = midi2_to_midi1(ump);
  if (single.message_type() == UmpMessageType::kMidi1ChannelVoice && single.word_count != 0) {
    out.messages[0] = single;
    out.count = 1;
  }
  return out;
}

}  // namespace sonare::midi
