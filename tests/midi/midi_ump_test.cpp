/// @file midi_ump_test.cpp
/// @brief MIDI core: UMP encode/decode round-trips, MIDI 1.0 byte-stream
///        adapter round-trip, and MIDI 1.0 <-> 2.0 conversion with the
///        documented lossy velocity/CC scaling pinned to exact values.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/ump.h"

namespace {

using sonare::midi::Ump;
using sonare::midi::UmpMessageType;
using sonare::midi::UmpStatus;

uint8_t status_of(const Ump& u) { return u.status_nibble(); }

}  // namespace

TEST_CASE("UMP MIDI 1.0 channel-voice encode exposes the expected fields", "[midi]") {
  const Ump note_on = sonare::midi::make_midi1_note_on(/*group=*/2, /*channel=*/5,
                                                       /*note=*/60, /*velocity7=*/100);
  REQUIRE(note_on.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(status_of(note_on) == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE(note_on.channel() == 5);
  REQUIRE(note_on.group == 2);
  REQUIRE(note_on.note_number() == 60);
  REQUIRE(note_on.is_note_on());
  REQUIRE_FALSE(note_on.is_note_off());
  REQUIRE(note_on.word_count == 1);

  const Ump note_off = sonare::midi::make_midi1_note_off(2, 5, 60, 0);
  REQUIRE(note_off.is_note_off());
  REQUIRE_FALSE(note_off.is_note_on());
  REQUIRE(note_off.note_number() == 60);

  const Ump cc = sonare::midi::make_midi1_control_change(1, 3, /*controller=*/7, /*value7=*/64);
  REQUIRE(status_of(cc) == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(cc.channel() == 3);
  REQUIRE(((cc.words[0] >> 8) & 0x7Fu) == 7u);
  REQUIRE((cc.words[0] & 0x7Fu) == 64u);

  const Ump poly = sonare::midi::make_midi1_poly_pressure(1, 3, /*note=*/61, /*pressure7=*/70);
  REQUIRE(status_of(poly) == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly.note_number() == 61);
  REQUIRE((poly.words[0] & 0x7Fu) == 70u);

  const Ump pc = sonare::midi::make_midi1_program_change(0, 9, /*program=*/42);
  REQUIRE(status_of(pc) == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(pc.channel() == 9);
  REQUIRE(((pc.words[0] >> 8) & 0x7Fu) == 42u);

  const Ump pressure = sonare::midi::make_midi1_channel_pressure(0, 9, /*pressure7=*/88);
  REQUIRE(status_of(pressure) == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(((pressure.words[0] >> 8) & 0x7Fu) == 88u);

  const Ump bend = sonare::midi::make_midi1_pitch_bend(0, 9, /*bend14=*/0x1234u);
  REQUIRE(status_of(bend) == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(((bend.words[0] >> 8) & 0x7Fu) == 0x34u);
  REQUIRE((bend.words[0] & 0x7Fu) == 0x24u);
}

TEST_CASE("documented sonare_c_project_midi data0 packing yields a note-on", "[midi]") {
  // The public C-ABI header documents how a caller hand-packs a MIDI 1.0 note-on
  // into data0. A word built by that exact formula must be recognized as a
  // note-on and expose the packed fields, and must match make_midi1_note_on.
  const uint32_t group = 2;
  const uint32_t status = static_cast<uint32_t>(UmpStatus::kNoteOn);  // 0x9
  const uint32_t channel = 5;
  const uint32_t note = 60;
  const uint32_t velocity = 100;
  const uint32_t data0 =
      (0x2u << 28) | (group << 24) | (status << 20) | (channel << 16) | (note << 8) | velocity;

  Ump packed;
  packed.words[0] = data0;
  packed.word_count = 1;
  packed.group = static_cast<uint8_t>(group);

  REQUIRE(packed.is_note_on());
  REQUIRE(packed.channel() == channel);
  REQUIRE(packed.note_number() == note);
  REQUIRE(packed.words[0] == sonare::midi::make_midi1_note_on(
                                 static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                                 static_cast<uint8_t>(note), static_cast<uint8_t>(velocity))
                                 .words[0]);
}

TEST_CASE("MIDI 1.0 note-on velocity zero is treated as note-off", "[midi]") {
  const Ump zero_vel = sonare::midi::make_midi1_note_on(0, 0, 60, 0);
  REQUIRE(zero_vel.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(status_of(zero_vel) == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE_FALSE(zero_vel.is_note_on());
  REQUIRE(zero_vel.is_note_off());

  const Ump midi2_zero_vel = sonare::midi::make_midi2_note_on(0, 0, 60, 0);
  REQUIRE(midi2_zero_vel.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(midi2_zero_vel.is_note_on());
  REQUIRE_FALSE(midi2_zero_vel.is_note_off());
}

TEST_CASE("MIDI 1.0 -> 2.0 lowers a velocity-zero note-on to a note-off", "[midi]") {
  const Ump m1_zero = sonare::midi::make_midi1_note_on(2, 5, /*note=*/60, /*velocity7=*/0);
  const Ump m2 = sonare::midi::midi1_to_midi2(m1_zero);
  REQUIRE(m2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(m2.is_note_off());
  REQUIRE_FALSE(m2.is_note_on());

  const Ump m1_hit = sonare::midi::make_midi1_note_on(2, 5, 60, /*velocity7=*/64);
  const Ump m2_hit = sonare::midi::midi1_to_midi2(m1_hit);
  REQUIRE(m2_hit.is_note_on());
  REQUIRE_FALSE(m2_hit.is_note_off());
}

TEST_CASE("UMP MIDI 2.0 channel-voice encode exposes the expected fields", "[midi]") {
  const Ump note_on = sonare::midi::make_midi2_note_on(/*group=*/1, /*channel=*/4,
                                                       /*note=*/64, /*velocity16=*/0x8000u);
  REQUIRE(note_on.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(note_on.is_note_on());
  REQUIRE(note_on.channel() == 4);
  REQUIRE(note_on.note_number() == 64);
  REQUIRE(note_on.word_count == 2);
  REQUIRE(static_cast<uint16_t>(note_on.words[1] >> 16) == 0x8000u);

  const Ump note_off = sonare::midi::make_midi2_note_off(1, 4, 64, /*velocity16=*/0x4000u);
  REQUIRE(note_off.is_note_off());
  REQUIRE(static_cast<uint16_t>(note_off.words[1] >> 16) == 0x4000u);

  const Ump cc =
      sonare::midi::make_midi2_control_change(0, 2, /*controller=*/10, /*value32=*/0xDEADBEEFu);
  REQUIRE(status_of(cc) == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(cc.words[1] == 0xDEADBEEFu);

  const Ump poly = sonare::midi::make_midi2_poly_pressure(0, 2, /*note=*/62,
                                                          /*pressure32=*/0xCAFEBABEu);
  REQUIRE(status_of(poly) == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly.note_number() == 62);
  REQUIRE(poly.words[1] == 0xCAFEBABEu);

  const Ump pc = sonare::midi::make_midi2_program_change(0, 6, /*program=*/12, /*bank_msb=*/1,
                                                         /*bank_lsb=*/2, /*bank_valid=*/true);
  REQUIRE(status_of(pc) == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(((pc.words[1] >> 24) & 0x7Fu) == 12u);
  REQUIRE(((pc.words[1] >> 8) & 0x7Fu) == 1u);
  REQUIRE((pc.words[1] & 0x7Fu) == 2u);
  REQUIRE((pc.words[0] & 0x01u) == 0x01u);  // bank-valid flag in byte3.

  const Ump pressure = sonare::midi::make_midi2_channel_pressure(0, 6, 0x80000000u);
  REQUIRE(status_of(pressure) == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(pressure.words[1] == 0x80000000u);

  const Ump bend = sonare::midi::make_midi2_pitch_bend(0, 6, 0x80000000u);
  REQUIRE(status_of(bend) == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(bend.words[1] == 0x80000000u);

  const Ump pnc = sonare::midi::make_midi2_per_note_controller(0, 0, /*note=*/72, /*index=*/3,
                                                               /*value32=*/0x12345678u);
  REQUIRE(pnc.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(status_of(pnc) == static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController));
  REQUIRE(pnc.note_number() == 72);
  REQUIRE(((pnc.words[0]) & 0xFFu) == 3u);  // controller index in byte3.
  REQUIRE(pnc.words[1] == 0x12345678u);

  const Ump apnc =
      sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 73, 4, 0x23456789u);
  REQUIRE(status_of(apnc) == static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController));
  REQUIRE(apnc.note_number() == 73);
  REQUIRE((apnc.words[0] & 0xFFu) == 4u);
  REQUIRE(apnc.words[1] == 0x23456789u);

  const Ump rc = sonare::midi::make_midi2_registered_controller(0, 1, 2, 3, 0x3456789Au);
  REQUIRE(status_of(rc) == static_cast<uint8_t>(UmpStatus::kRegisteredController));
  REQUIRE(((rc.words[0] >> 8) & 0xFFu) == 2u);
  REQUIRE((rc.words[0] & 0xFFu) == 3u);
  REQUIRE(rc.words[1] == 0x3456789Au);

  const Ump ac = sonare::midi::make_midi2_assignable_controller(0, 1, 4, 5, 0x456789ABu);
  REQUIRE(status_of(ac) == static_cast<uint8_t>(UmpStatus::kAssignableController));
  REQUIRE(((ac.words[0] >> 8) & 0xFFu) == 4u);
  REQUIRE((ac.words[0] & 0xFFu) == 5u);
  REQUIRE(ac.words[1] == 0x456789ABu);
}

TEST_CASE("UMP SysEx handle carries the handle id without inline payload", "[midi]") {
  const Ump sx = sonare::midi::make_sysex_handle(/*group=*/3, /*handle=*/0xABCDu);
  REQUIRE(sx.message_type() == UmpMessageType::kData64);
  REQUIRE(sx.group == 3);
  REQUIRE(sx.sysex_handle == 0xABCDu);
}

TEST_CASE("SysExStore owns variable-length payloads behind UMP handles", "[midi]") {
  sonare::midi::SysExStore store;
  const std::vector<uint8_t> payload = {0xF0u, 0x7Eu, 0x7Fu, 0x09u, 0x01u, 0xF7u};

  const sonare::midi::SysExHandle handle = store.add(payload);
  REQUIRE(handle != 0);
  REQUIRE(store.size() == 1);
  REQUIRE(store.contains(handle));

  const std::vector<uint8_t>* stored = store.lookup(handle);
  REQUIRE(stored != nullptr);
  REQUIRE(*stored == payload);

  const Ump sx = sonare::midi::make_sysex_handle(/*group=*/2, handle);
  REQUIRE(sx.sysex_handle == handle);
  REQUIRE(sx.group == 2);

  REQUIRE(store.lookup(0) == nullptr);
  REQUIRE(store.add(nullptr, payload.size()) == 0);
  REQUIRE(store.add(payload.data(), 0) == 0);

  REQUIRE(store.remove(handle));
  REQUIRE_FALSE(store.contains(handle));
  REQUIRE_FALSE(store.remove(handle));

  const sonare::midi::SysExHandle second = store.add(payload);
  REQUIRE(second != 0);
  store.clear();
  REQUIRE(store.size() == 0);
  REQUIRE(store.lookup(second) == nullptr);
}

namespace {
// Decode one SysEx7 (MT=0x3) UMP data message back to its payload bytes, the
// inverse of sysex7_payload_to_umps. Mirrors the SMF2 importer's reader.
uint8_t sysex7_status(const Ump& u) { return static_cast<uint8_t>((u.words[0] >> 20) & 0x0Fu); }
void append_sysex7(const Ump& u, std::vector<uint8_t>* out) {
  const uint8_t num = static_cast<uint8_t>((u.words[0] >> 16) & 0x0Fu);
  const uint8_t bytes[6] = {static_cast<uint8_t>((u.words[0] >> 8) & 0xFFu),
                            static_cast<uint8_t>(u.words[0] & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 24) & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 16) & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 8) & 0xFFu),
                            static_cast<uint8_t>(u.words[1] & 0xFFu)};
  for (uint8_t i = 0; i < num && i < 6; ++i) out->push_back(bytes[i]);
}
}  // namespace

TEST_CASE("SysEx7 payload packetizes into UMP data messages and round-trips", "[midi]") {
  // Short payload (<= 6 data bytes) => a single Complete packet.
  {
    const std::vector<uint8_t> payload = {0x7Eu, 0x7Fu, 0x09u, 0x01u};  // 4 bytes, all 7-bit
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(),
                                                              /*group=*/5, out.data(), out.size());
    REQUIRE(count == 1);
    REQUIRE(out[0].message_type() == UmpMessageType::kData64);
    REQUIRE(out[0].group == 5);
    REQUIRE(sysex7_status(out[0]) == 0x0u);  // Complete
    std::vector<uint8_t> decoded;
    append_sysex7(out[0], &decoded);
    REQUIRE(decoded == payload);
  }

  // Multi-packet payload (13 bytes => 6 + 6 + 1) => Start / Continue / End.
  {
    std::vector<uint8_t> payload;
    for (uint8_t i = 0; i < 13; ++i) payload.push_back(static_cast<uint8_t>(i + 1));  // 7-bit
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(),
                                                              /*group=*/0, out.data(), out.size());
    REQUIRE(count == 3);
    REQUIRE(sysex7_status(out[0]) == 0x1u);  // Start
    REQUIRE(sysex7_status(out[1]) == 0x2u);  // Continue
    REQUIRE(sysex7_status(out[2]) == 0x3u);  // End
    std::vector<uint8_t> decoded;
    for (size_t k = 0; k < count; ++k) append_sysex7(out[k], &decoded);
    REQUIRE(decoded == payload);
  }

  // A leading 0xF0 / trailing 0xF7 frame is stripped before packetizing.
  {
    const std::vector<uint8_t> framed = {0xF0u, 0x41u, 0x10u, 0x42u, 0xF7u};
    const std::vector<uint8_t> inner = {0x41u, 0x10u, 0x42u};
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(framed.data(), framed.size(),
                                                              /*group=*/1, out.data(), out.size());
    REQUIRE(count == 1);
    std::vector<uint8_t> decoded;
    append_sysex7(out[0], &decoded);
    REQUIRE(decoded == inner);
  }
}

TEST_CASE("SysEx7 packetizer rejects empty, 8-bit, and over-capacity payloads", "[midi]") {
  std::array<Ump, 8> out{};
  // Empty payload (also empty after stripping F0/F7) => 0.
  REQUIRE(sonare::midi::sysex7_payload_to_umps(nullptr, 0, 0, out.data(), out.size()) == 0);
  const std::vector<uint8_t> framed_only = {0xF0u, 0xF7u};
  REQUIRE(sonare::midi::sysex7_payload_to_umps(framed_only.data(), framed_only.size(), 0,
                                               out.data(), out.size()) == 0);
  // An 8-bit data byte is not valid SysEx7 (needs SysEx8) => 0.
  const std::vector<uint8_t> eight_bit = {0x41u, 0x80u, 0x42u};
  REQUIRE(sonare::midi::sysex7_payload_to_umps(eight_bit.data(), eight_bit.size(), 0, out.data(),
                                               out.size()) == 0);
  // Buffer too small for the required packet count => 0 (no partial SysEx).
  std::vector<uint8_t> big(13, 0x01u);
  std::array<Ump, 2> tiny{};  // needs 3 packets, only 2 fit
  REQUIRE(sonare::midi::sysex7_payload_to_umps(big.data(), big.size(), 0, tiny.data(),
                                               tiny.size()) == 0);
}

TEST_CASE("stateful SysEx7 packetizer streams beyond the 1536-byte backend boundary", "[midi]") {
  std::array<Ump, 256> scratch{};

  std::vector<uint8_t> boundary(1536);
  for (size_t i = 0; i < boundary.size(); ++i) boundary[i] = static_cast<uint8_t>(i % 0x80u);
  REQUIRE(sonare::midi::sysex7_payload_to_umps(boundary.data(), boundary.size(), 2, scratch.data(),
                                               scratch.size()) == 256);

  std::vector<uint8_t> payload(4097);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i + 1u) % 0x80u);
  REQUIRE(sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(), 2, scratch.data(),
                                               scratch.size()) == 0);

  sonare::midi::SysEx7Packetizer packetizer(payload.data(), payload.size(), /*group=*/2);
  REQUIRE(packetizer.valid());
  REQUIRE(packetizer.packet_count() == 683);
  std::vector<uint8_t> decoded;
  size_t batches = 0;
  size_t last_batch_count = 0;
  while (packetizer.remaining_packets() > 0) {
    const size_t count = packetizer.read(scratch.data(), scratch.size());
    REQUIRE(count > 0);
    for (size_t i = 0; i < count; ++i) append_sysex7(scratch[i], &decoded);
    last_batch_count = count;
    ++batches;
  }
  REQUIRE(batches == 3);
  REQUIRE(decoded == payload);
  REQUIRE(last_batch_count > 0);
  REQUIRE(sysex7_status(scratch[last_batch_count - 1u]) == 0x3u);

  REQUIRE(packetizer.seek_packet(256));
  REQUIRE(packetizer.packet_position() == 256);
  REQUIRE(packetizer.read(scratch.data(), 1) == 1);
  REQUIRE(sysex7_status(scratch[0]) == 0x2u);
  REQUIRE_FALSE(packetizer.seek_packet(packetizer.packet_count() + 1u));
}

TEST_CASE("MIDI 1.0 byte-stream <-> UMP adapter round-trips channel-voice", "[midi]") {
  uint8_t running = 0;
  // Note-on, channel 5, note 60, velocity 100.
  const uint8_t bytes[] = {0x95u, 60u, 100u};
  Ump out;
  const size_t consumed =
      sonare::midi::midi1_bytes_to_ump(bytes, sizeof(bytes), /*group=*/0, &running, &out);
  REQUIRE(consumed == 3);
  REQUIRE(out.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(out.is_note_on());
  REQUIRE(out.channel() == 5);
  REQUIRE(out.note_number() == 60);
  REQUIRE((out.words[0] & 0x7Fu) == 100u);
  REQUIRE(running == 0x95u);

  uint8_t serialized[3] = {0, 0, 0};
  const size_t written = sonare::midi::ump_to_midi1_bytes(out, serialized, sizeof(serialized));
  REQUIRE(written == 3);
  REQUIRE(serialized[0] == bytes[0]);
  REQUIRE(serialized[1] == bytes[1]);
  REQUIRE(serialized[2] == bytes[2]);

  // Running status: a second note-on with no leading status byte reuses 0x95.
  const uint8_t running_bytes[] = {62u, 90u};
  Ump out2;
  const size_t consumed2 =
      sonare::midi::midi1_bytes_to_ump(running_bytes, sizeof(running_bytes), 0, &running, &out2);
  REQUIRE(consumed2 == 2);
  REQUIRE(out2.is_note_on());
  REQUIRE(out2.channel() == 5);
  REQUIRE(out2.note_number() == 62);

  // Program change is a one-data-byte message.
  uint8_t pc_running = 0;
  const uint8_t pc_bytes[] = {0xC3u, 42u};
  Ump pc_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(pc_bytes, sizeof(pc_bytes), 0, &pc_running, &pc_out) ==
          2);
  REQUIRE(pc_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  uint8_t pc_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(pc_out, pc_serialized, sizeof(pc_serialized)) == 2);
  REQUIRE(pc_serialized[0] == 0xC3u);
  REQUIRE(pc_serialized[1] == 42u);

  // Channel pressure is also a one-data-byte message.
  uint8_t cp_running = 0;
  const uint8_t cp_bytes[] = {0xD2u, 99u};
  Ump cp_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(cp_bytes, sizeof(cp_bytes), 0, &cp_running, &cp_out) ==
          2);
  REQUIRE(cp_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  uint8_t cp_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(cp_out, cp_serialized, sizeof(cp_serialized)) == 2);
  REQUIRE(cp_serialized[0] == 0xD2u);
  REQUIRE(cp_serialized[1] == 99u);

  // Pitch bend preserves 14-bit LSB/MSB ordering.
  uint8_t bend_running = 0;
  const uint8_t bend_bytes[] = {0xE1u, 0x34u, 0x12u};
  Ump bend_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(bend_bytes, sizeof(bend_bytes), 0, &bend_running,
                                           &bend_out) == 3);
  REQUIRE(bend_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend));
  uint8_t bend_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(bend_out, bend_serialized, sizeof(bend_serialized)) ==
          3);
  REQUIRE(bend_serialized[0] == 0xE1u);
  REQUIRE(bend_serialized[1] == 0x34u);
  REQUIRE(bend_serialized[2] == 0x12u);
}

TEST_CASE("MIDI 1.0 byte adapter rejects system messages and incomplete buffers", "[midi]") {
  uint8_t running = 0;
  Ump out;
  const uint8_t sysex_start[] = {0xF0u, 0x7Eu};
  REQUIRE(sonare::midi::midi1_bytes_to_ump(sysex_start, sizeof(sysex_start), 0, &running, &out) ==
          0);
  // Incomplete note-on (missing velocity byte).
  const uint8_t partial[] = {0x90u, 60u};
  REQUIRE(sonare::midi::midi1_bytes_to_ump(partial, sizeof(partial), 0, &running, &out) == 0);
}

TEST_CASE("MIDI 1.0 -> 2.0 -> 1.0 channel-voice round-trips losslessly (top bits)", "[midi]") {
  const Ump m1 = sonare::midi::make_midi1_note_on(0, 0, 60, /*velocity7=*/100);
  const Ump m2 = sonare::midi::midi1_to_midi2(m1);
  REQUIRE(m2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  const Ump back = sonare::midi::midi2_to_midi1(m2);
  REQUIRE(back.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(back.is_note_on());
  REQUIRE(back.channel() == 0);
  REQUIRE(back.note_number() == 60);
  REQUIRE((back.words[0] & 0x7Fu) == 100u);  // velocity survives the round-trip.

  // Control change round-trip (top 7 CC bits survive).
  const Ump cc1 = sonare::midi::make_midi1_control_change(1, 3, 7, /*value7=*/64);
  const Ump cc_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(cc1));
  REQUIRE(cc_back.channel() == 3);
  REQUIRE(((cc_back.words[0] >> 8) & 0x7Fu) == 7u);
  REQUIRE((cc_back.words[0] & 0x7Fu) == 64u);

  // Program change round-trip.
  const Ump pc1 = sonare::midi::make_midi1_program_change(0, 9, 42);
  const Ump pc_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(pc1));
  REQUIRE(pc_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(pc_back.channel() == 9);
  REQUIRE(((pc_back.words[0] >> 8) & 0x7Fu) == 42u);

  const Ump poly1 = sonare::midi::make_midi1_poly_pressure(0, 4, 61, 77);
  const Ump poly_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(poly1));
  REQUIRE(poly_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly_back.channel() == 4);
  REQUIRE(poly_back.note_number() == 61);
  REQUIRE((poly_back.words[0] & 0x7Fu) == 77u);

  const Ump pressure1 = sonare::midi::make_midi1_channel_pressure(0, 4, 88);
  const Ump pressure_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(pressure1));
  REQUIRE(pressure_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(((pressure_back.words[0] >> 8) & 0x7Fu) == 88u);

  const Ump bend1 = sonare::midi::make_midi1_pitch_bend(0, 4, 0x1234u);
  const Ump bend_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(bend1));
  REQUIRE(bend_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(((bend_back.words[0] >> 8) & 0x7Fu) == 0x34u);
  REQUIRE((bend_back.words[0] & 0x7Fu) == 0x24u);
}

TEST_CASE("MIDI 1.0 -> 2.0 velocity/CC up-scale pins the min-center-max values", "[midi]") {
  // Velocity 7->16: documented MIDI-Association min-center-max bit scaling.
  REQUIRE(sonare::midi::scale_velocity_7_to_16(0) == 0u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(1) == 512u);      // 1 << 9.
  REQUIRE(sonare::midi::scale_velocity_7_to_16(64) == 0x8000u);  // center maps to 32768.
  REQUIRE(sonare::midi::scale_velocity_7_to_16(100) == 51488u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(127) == 65528u);  // max, not 65535.

  // CC 7->32.
  REQUIRE(sonare::midi::scale_cc_7_to_32(64) == 0x80000000u);  // 2147483648, center.
  REQUIRE(sonare::midi::scale_cc_7_to_32(100) == 3374317568u);
  REQUIRE(sonare::midi::scale_cc_7_to_32(127) == 4294443008u);  // max, not 0xFFFFFFFF.

  // The note-on path embeds the up-scaled 16-bit velocity in word[1].
  const Ump m2 = sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_note_on(0, 0, 60, 127));
  REQUIRE(static_cast<uint16_t>(m2.words[1] >> 16) == 65528u);
  const Ump cc2 =
      sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_control_change(0, 0, 7, 127));
  REQUIRE(cc2.words[1] == 4294443008u);
}

TEST_CASE("MIDI 2.0 -> 1.0 velocity/CC down-scale is the top-7-bit truncation", "[midi]") {
  // Down-scale takes the top 7 bits (>> 9 for velocity, >> 25 for CC). LOSSY in
  // the low bits: a 2.0 velocity whose low 9 bits are non-zero loses them.
  REQUIRE(sonare::midi::scale_velocity_16_to_7(0x8000u) == 64u);
  REQUIRE(sonare::midi::scale_velocity_16_to_7(65535u) == 127u);
  REQUIRE(sonare::midi::scale_velocity_16_to_7(0x81FFu) == 64u);  // low 9 bits dropped.

  const Ump quiet_on = sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_on(0, 0, 60, 1));
  REQUIRE(quiet_on.is_note_on());
  REQUIRE((quiet_on.words[0] & 0x7Fu) == 1u);
  const Ump silent_on = sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_on(0, 0, 60, 0));
  REQUIRE(status_of(silent_on) == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE((silent_on.words[0] & 0x7Fu) == 0u);
  const Ump quiet_off =
      sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_off(0, 0, 60, 1));
  REQUIRE(quiet_off.is_note_off());
  REQUIRE((quiet_off.words[0] & 0x7Fu) == 0u);

  REQUIRE(sonare::midi::scale_cc_32_to_7(0x80000000u) == 64u);
  REQUIRE(sonare::midi::scale_cc_32_to_7(0xFFFFFFFFu) == 127u);
  REQUIRE(sonare::midi::scale_cc_32_to_7(0x81FFFFFFu) == 64u);  // low 25 bits dropped.

  REQUIRE(sonare::midi::scale_bend_14_to_32(0) == 0u);
  REQUIRE(sonare::midi::scale_bend_14_to_32(8192) == 0x80000000u);
  REQUIRE(sonare::midi::scale_bend_32_to_14(0x80000000u) == 8192u);
  REQUIRE(sonare::midi::scale_bend_32_to_14(0xFFFFFFFFu) == 16383u);
}

TEST_CASE("MIDI 2.0 per-note controller down-converts to a dropped (empty) UMP", "[midi]") {
  const Ump pnc = sonare::midi::make_midi2_per_note_controller(0, 0, 60, 1, 0x1234u);
  const Ump dropped = sonare::midi::midi2_to_midi1(pnc);
  REQUIRE(dropped.word_count == 0);  // no MIDI 1.0 equivalent: caller drops it.

  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 60, 1, 0x1234u))
              .word_count == 0);
  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_registered_controller(0, 0, 1, 2, 0x1234u))
              .word_count == 0);
  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_assignable_controller(0, 0, 1, 2, 0x1234u))
              .word_count == 0);
}

TEST_CASE("MIDI 2.0 banked program change lowers to MIDI 1.0 bank select and program", "[midi]") {
  const Ump pc = sonare::midi::make_midi2_program_change(2, 5, /*program=*/42, /*bank_msb=*/0x79,
                                                         /*bank_lsb=*/3, /*bank_valid=*/true);
  const auto lowered = sonare::midi::midi2_to_midi1_messages(pc);

  REQUIRE(lowered.count == 3);
  REQUIRE(lowered.messages[0].status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(lowered.messages[0].channel() == 5);
  REQUIRE(((lowered.messages[0].words[0] >> 8) & 0x7Fu) == 0u);
  REQUIRE((lowered.messages[0].words[0] & 0x7Fu) == 0x79u);

  REQUIRE(lowered.messages[1].status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(((lowered.messages[1].words[0] >> 8) & 0x7Fu) == 32u);
  REQUIRE((lowered.messages[1].words[0] & 0x7Fu) == 3u);

  REQUIRE(lowered.messages[2].status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(((lowered.messages[2].words[0] >> 8) & 0x7Fu) == 42u);
}
