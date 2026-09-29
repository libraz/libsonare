/// @file channel_voice_decode_test.cpp
/// @brief Tests for decode_channel_voice across both protocols, every status and value class.

#include "midi/channel_voice_decode.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <type_traits>

#include "midi/ump.h"

using namespace sonare::midi;

namespace {

static_assert(std::is_trivially_copyable_v<ChannelVoiceEvent>);

ChannelVoiceEvent decode_ok(const Ump& u) {
  ChannelVoiceEvent ev;
  REQUIRE(decode_channel_voice(u, &ev));
  return ev;
}

Ump raw_ump(uint32_t w0, uint32_t w1, uint8_t words) {
  Ump u;
  u.words[0] = w0;
  u.words[1] = w1;
  u.word_count = words;
  u.group = static_cast<uint8_t>((w0 >> 24) & 0xFu);
  return u;
}

}  // namespace

TEST_CASE("channel voice decode: MIDI 2.0 statuses map to kinds with raw values", "[midi][midi2]") {
  struct Row {
    Ump ump;
    ChannelVoiceKind kind;
  };
  const Row rows[] = {
      {make_midi2_per_note_controller(3, 5, 60, 9, 0xDEADBEEFu),
       ChannelVoiceKind::RegisteredPerNote},
      {make_midi2_assignable_per_note_controller(3, 5, 60, 9, 0xDEADBEEFu),
       ChannelVoiceKind::AssignablePerNote},
      {make_midi2_registered_controller(3, 5, 0, 7, 0xDEADBEEFu),
       ChannelVoiceKind::RegisteredController},
      {make_midi2_assignable_controller(3, 5, 0, 7, 0xDEADBEEFu),
       ChannelVoiceKind::AssignableController},
      {make_midi2_relative_registered_controller(3, 5, 0, 7, 0xDEADBEEFu),
       ChannelVoiceKind::RelativeRegistered},
      {make_midi2_relative_assignable_controller(3, 5, 0, 7, 0xDEADBEEFu),
       ChannelVoiceKind::RelativeAssignable},
      {make_midi2_per_note_pitch_bend(3, 5, 60, 0xDEADBEEFu), ChannelVoiceKind::PerNotePitchBend},
      {make_midi2_note_off(3, 5, 60, 0xBEEFu), ChannelVoiceKind::NoteOff},
      {make_midi2_note_on(3, 5, 60, 0xBEEFu), ChannelVoiceKind::NoteOn},
      {make_midi2_poly_pressure(3, 5, 60, 0xDEADBEEFu), ChannelVoiceKind::PolyPressure},
      {make_midi2_control_change(3, 5, 60, 0xDEADBEEFu), ChannelVoiceKind::ControlChange},
      {make_midi2_program_change(3, 5, 10, 1, 2, true), ChannelVoiceKind::ProgramChange},
      {make_midi2_channel_pressure(3, 5, 0xDEADBEEFu), ChannelVoiceKind::ChannelPressure},
      {make_midi2_pitch_bend(3, 5, 0xDEADBEEFu), ChannelVoiceKind::PitchBend},
      {make_midi2_per_note_management(3, 5, 60, true, true), ChannelVoiceKind::PerNoteManagement},
  };
  for (const Row& r : rows) {
    const ChannelVoiceEvent ev = decode_ok(r.ump);
    CHECK(ev.kind == r.kind);
    CHECK(ev.group == 3);
    CHECK(ev.channel == 5);
    switch (r.kind) {
      case ChannelVoiceKind::RegisteredPerNote:
      case ChannelVoiceKind::AssignablePerNote:
        CHECK(ev.note == 60);
        CHECK(ev.index == 9);
        CHECK(ev.value.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::RegisteredController:
      case ChannelVoiceKind::AssignableController:
      case ChannelVoiceKind::RelativeRegistered:
      case ChannelVoiceKind::RelativeAssignable:
        CHECK(ev.bank == 0);
        CHECK(ev.index == 7);
        CHECK(ev.value.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::PerNotePitchBend:
        CHECK(ev.note == 60);
        CHECK(ev.bend.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::NoteOff:
      case ChannelVoiceKind::NoteOn:
        CHECK(ev.note == 60);
        CHECK(ev.velocity.raw == 0xBEEFu);
        break;
      case ChannelVoiceKind::PolyPressure:
      case ChannelVoiceKind::ControlChange:
        CHECK(ev.note == 60);
        CHECK(ev.value.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::ProgramChange:
        CHECK(ev.program == 10);
        CHECK(ev.bank_msb == 1);
        CHECK(ev.bank_lsb == 2);
        CHECK((ev.flags & 1u) == 1u);
        break;
      case ChannelVoiceKind::ChannelPressure:
        CHECK(ev.value.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::PitchBend:
        CHECK(ev.bend.raw == 0xDEADBEEFu);
        break;
      case ChannelVoiceKind::PerNoteManagement:
        CHECK(ev.note == 60);
        CHECK(ev.flags == 3);
        break;
    }
  }
}

TEST_CASE("channel voice decode: MIDI 1.0 statuses map to kinds", "[midi][midi1]") {
  const ChannelVoiceEvent off = decode_ok(make_midi1_note_off(1, 2, 64, 33));
  CHECK(off.kind == ChannelVoiceKind::NoteOff);
  CHECK(off.note == 64);
  CHECK(off.velocity.u7() == 33);
  CHECK(off.group == 1);
  CHECK(off.channel == 2);

  const ChannelVoiceEvent on = decode_ok(make_midi1_note_on(1, 2, 64, 100));
  CHECK(on.kind == ChannelVoiceKind::NoteOn);
  CHECK(on.velocity.u7() == 100);

  const ChannelVoiceEvent poly = decode_ok(make_midi1_poly_pressure(1, 2, 64, 77));
  CHECK(poly.kind == ChannelVoiceKind::PolyPressure);
  CHECK(poly.note == 64);
  CHECK(poly.value.u7() == 77);

  const ChannelVoiceEvent cc = decode_ok(make_midi1_control_change(1, 2, 74, 99));
  CHECK(cc.kind == ChannelVoiceKind::ControlChange);
  CHECK(cc.note == 74);
  CHECK(cc.value.u7() == 99);

  const ChannelVoiceEvent pc = decode_ok(make_midi1_program_change(1, 2, 42));
  CHECK(pc.kind == ChannelVoiceKind::ProgramChange);
  CHECK(pc.program == 42);
  CHECK((pc.flags & 1u) == 0u);

  const ChannelVoiceEvent ch = decode_ok(make_midi1_channel_pressure(1, 2, 55));
  CHECK(ch.kind == ChannelVoiceKind::ChannelPressure);
  CHECK(ch.value.u7() == 55);

  const ChannelVoiceEvent pb = decode_ok(make_midi1_pitch_bend(1, 2, 0x1234));
  CHECK(pb.kind == ChannelVoiceKind::PitchBend);
  CHECK(pb.bend.u14() == 0x1234);
}

TEST_CASE("channel voice decode: velocity zero by protocol", "[midi][midi1][midi2]") {
  const ChannelVoiceEvent m1 = decode_ok(make_midi1_note_on(0, 0, 60, 0));
  CHECK(m1.kind == ChannelVoiceKind::NoteOff);
  CHECK(m1.velocity.raw == 0x8000);

  const ChannelVoiceEvent m2 = decode_ok(make_midi2_note_on(0, 0, 60, 0));
  CHECK(m2.kind == ChannelVoiceKind::NoteOn);
  CHECK(m2.velocity.raw == 0);

  const ChannelVoiceEvent m2off = decode_ok(make_midi2_note_off(0, 0, 60, 0));
  CHECK(m2off.kind == ChannelVoiceKind::NoteOff);
  CHECK(m2off.velocity.raw == 0);
}

TEST_CASE("channel voice decode: MIDI 1.0 bank and RPN controllers stay plain CC",
          "[midi][midi1]") {
  for (uint8_t cc : {0, 32, 6, 38, 98, 99, 100, 101}) {
    const ChannelVoiceEvent ev = decode_ok(make_midi1_control_change(0, 0, cc, 5));
    CHECK(ev.kind == ChannelVoiceKind::ControlChange);
    CHECK(ev.note == cc);
    CHECK(ev.value.u7() == 5);
  }
}

TEST_CASE("channel voice decode: note attributes", "[midi][midi2]") {
  for (uint8_t type : {0, 1, 2, 3, 0x80}) {
    for (uint16_t data : {0x0000, 0x0001, 0x1234, 0xFFFF}) {
      for (bool on : {true, false}) {
        const Ump u = on ? make_midi2_note_on(0, 1, 40, 0x4000, type, data)
                         : raw_ump(make_midi2_note_off(0, 1, 40, 0x4000).words[0] | type,
                                   (0x4000u << 16) | data, 2);
        const ChannelVoiceEvent ev = decode_ok(u);
        CHECK(ev.kind == (on ? ChannelVoiceKind::NoteOn : ChannelVoiceKind::NoteOff));
        CHECK(ev.index == type);
        CHECK(ev.attribute_data == data);
        CHECK(ev.velocity.raw == 0x4000);
      }
    }
  }
}

TEST_CASE("channel voice decode: per-note management flags", "[midi][midi2]") {
  for (bool detach : {false, true}) {
    for (bool reset : {false, true}) {
      const ChannelVoiceEvent ev =
          decode_ok(make_midi2_per_note_management(0, 0, 61, detach, reset));
      CHECK(ev.kind == ChannelVoiceKind::PerNoteManagement);
      CHECK(ev.note == 61);
      CHECK(((ev.flags & 1u) != 0) == reset);
      CHECK(((ev.flags & 2u) != 0) == detach);
    }
  }
}

TEST_CASE("channel voice decode: relative controller keeps two's complement delta",
          "[midi][midi2]") {
  const uint32_t neg = static_cast<uint32_t>(-1000);
  const ChannelVoiceEvent rr =
      decode_ok(make_midi2_relative_registered_controller(0, 0, 0, 2, neg));
  CHECK(rr.kind == ChannelVoiceKind::RelativeRegistered);
  CHECK(static_cast<int32_t>(rr.value.raw) == -1000);
  const ChannelVoiceEvent ra =
      decode_ok(make_midi2_relative_assignable_controller(0, 0, 3, 4, 250));
  CHECK(ra.kind == ChannelVoiceKind::RelativeAssignable);
  CHECK(ra.bank == 3);
  CHECK(ra.index == 4);
  CHECK(static_cast<int32_t>(ra.value.raw) == 250);
}

TEST_CASE("channel voice decode: program change bank-valid", "[midi][midi2]") {
  const ChannelVoiceEvent with = decode_ok(make_midi2_program_change(0, 0, 9, 20, 30, true));
  CHECK((with.flags & 1u) == 1u);
  CHECK(with.program == 9);
  CHECK(with.bank_msb == 20);
  CHECK(with.bank_lsb == 30);
  const ChannelVoiceEvent without = decode_ok(make_midi2_program_change(0, 0, 9, 20, 30, false));
  CHECK((without.flags & 1u) == 0u);
  CHECK(without.program == 9);
}

TEST_CASE("channel voice decode: rejected messages", "[midi][midi1][midi2]") {
  ChannelVoiceEvent ev;
  ev.channel = 9;

  // Reserved MIDI 2.0 status 0x7.
  CHECK_FALSE(decode_channel_voice(raw_ump(0x40700000u, 0, 2), &ev));
  // MIDI 1.0 status nibbles below 0x8.
  for (uint32_t s = 0; s < 8; ++s) {
    CHECK_FALSE(decode_channel_voice(raw_ump(0x20000000u | (s << 20), 0, 1), &ev));
  }
  // Non channel-voice message types.
  for (uint32_t mt : {0x0u, 0x1u, 0x3u, 0x5u, 0xDu, 0xFu}) {
    CHECK_FALSE(decode_channel_voice(raw_ump((mt << 28) | 0x00900000u, 0, 2), &ev));
  }
  CHECK(ev.channel == 9);
}

TEST_CASE("channel voice decode: directly held registered controllers", "[midi][midi2]") {
  for (uint8_t bank : {0, 1}) {
    for (uint8_t index : {0, 1, 2, 3, 6, 7, 8}) {
      const bool expected = bank == 0 && (index <= 2 || index == 7);
      CHECK(is_directly_held_registered_controller(bank, index) == expected);
    }
  }
}

TEST_CASE("channel voice decode: MIDI 1.0 values read back exactly", "[midi][midi1]") {
  for (uint32_t v = 0; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    CHECK(decode_ok(make_midi1_control_change(0, 0, 1, v7)).value.u7() == v7);
    CHECK(decode_ok(make_midi1_poly_pressure(0, 0, 1, v7)).value.u7() == v7);
    CHECK(decode_ok(make_midi1_channel_pressure(0, 0, v7)).value.u7() == v7);
    if (v >= 1) {
      CHECK(decode_ok(make_midi1_note_on(0, 0, 1, v7)).velocity.u7() == v7);
      CHECK(decode_ok(make_midi1_note_off(0, 0, 1, v7)).velocity.u7() == v7);
    }
  }
  for (uint32_t v = 0; v < 16384; ++v) {
    CHECK(decode_ok(make_midi1_pitch_bend(0, 0, static_cast<uint16_t>(v))).bend.u14() == v);
  }
}
