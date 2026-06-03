/// @file program_map_test.cpp
/// @brief MIDI routing: GM/GM2/drum/CC name lookups, bank+program round-trip,
///        and DestinationTable add/lookup/remove with out-of-range safety.

#include "midi/program_map.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

#include "midi/sound_destination.h"
#include "midi/ump.h"

namespace {

using sonare::midi::BankProgramMessages;
using sonare::midi::cc_index_for_name;
using sonare::midi::cc_name;
using sonare::midi::convert_for_destination;
using sonare::midi::destination_emits_midi2;
using sonare::midi::DestinationKind;
using sonare::midi::DestinationTable;
using sonare::midi::DrumMapOverride;
using sonare::midi::gm2_drum_name;
using sonare::midi::gm2_drum_set_name;
using sonare::midi::gm2_instrument_name;
using sonare::midi::gm_drum_name;
using sonare::midi::gm_drum_note_for_name;
using sonare::midi::gm_family_first_program;
using sonare::midi::gm_family_name;
using sonare::midi::gm_instrument_name;
using sonare::midi::gm_program_for_name;
using sonare::midi::make_midi1_note_on;
using sonare::midi::make_midi2_note_on;
using sonare::midi::per_note_controller_name;
using sonare::midi::program_to_messages;
using sonare::midi::ProgramSelection;
using sonare::midi::ProgramState;
using sonare::midi::remap_drum_note;
using sonare::midi::SoundDestination;
using sonare::midi::Ump;
using sonare::midi::UmpMessageType;
using sonare::midi::UmpStatus;

}  // namespace

TEST_CASE("GM instrument name forward and reverse lookup", "[midi]") {
  REQUIRE(gm_instrument_name(0) == "Acoustic Grand Piano");
  REQUIRE(gm_instrument_name(40) == "Violin");
  REQUIRE(gm_instrument_name(56) == "Trumpet");
  REQUIRE(gm_instrument_name(127) == "Gunshot");

  REQUIRE(gm_program_for_name("Acoustic Grand Piano") == 0);
  REQUIRE(gm_program_for_name("Violin") == 40);
  REQUIRE(gm_program_for_name("Gunshot") == 127);

  // Round-trip every program through name and back.
  for (int p = 0; p < 128; ++p) {
    const std::string_view name = gm_instrument_name(static_cast<uint8_t>(p));
    REQUIRE_FALSE(name.empty());
    REQUIRE(gm_program_for_name(name) == p);
  }
}

TEST_CASE("GM instrument out-of-range and unknown name are safe", "[midi]") {
  REQUIRE(gm_instrument_name(128).empty());
  REQUIRE(gm_instrument_name(200).empty());
  REQUIRE(gm_instrument_name(255).empty());
  REQUIRE(gm_program_for_name("Not A Real Instrument") == -1);
  REQUIRE(gm_program_for_name("") == -1);
}

TEST_CASE("GM families cover 16 families at 8-program boundaries", "[midi]") {
  REQUIRE(gm_family_name(0) == "Piano");
  REQUIRE(gm_family_name(4) == "Bass");
  REQUIRE(gm_family_name(15) == "Sound Effects");
  REQUIRE(gm_family_name(16).empty());

  REQUIRE(gm_family_first_program(0) == 0);
  REQUIRE(gm_family_first_program(4) == 32);
  REQUIRE(gm_family_first_program(15) == 120);
  REQUIRE(gm_family_first_program(16) == -1);
}

TEST_CASE("GM2 falls back to GM at bank LSB 0 and resolves variations", "[midi]") {
  // Bank LSB 0 == GM Level 1 compatible.
  for (int p = 0; p < 128; ++p) {
    REQUIRE(gm2_instrument_name(0, static_cast<uint8_t>(p)) ==
            gm_instrument_name(static_cast<uint8_t>(p)));
  }
  // Known variations.
  REQUIRE(gm2_instrument_name(1, 0) == "Acoustic Grand Piano (wide)");
  REQUIRE(gm2_instrument_name(1, 24) == "Ukulele");
  REQUIRE(gm2_instrument_name(1, 25) == "12-String Guitar");
  // Unknown variation falls back to the base GM name.
  REQUIRE(gm2_instrument_name(7, 0) == gm_instrument_name(0));
  // Out-of-range program is empty.
  REQUIRE(gm2_instrument_name(0, 200).empty());
}

TEST_CASE("GM2 melodic variation table is complete enough for representatives", "[midi]") {
  // Representative variations across families from the full GM2 set.
  REQUIRE(gm2_instrument_name(2, 0) == "Acoustic Grand Piano (dark)");
  REQUIRE(gm2_instrument_name(3, 4) == "60's Electric Piano");
  REQUIRE(gm2_instrument_name(2, 16) == "60's Organ 1");
  REQUIRE(gm2_instrument_name(2, 25) == "Mandolin");
  REQUIRE(gm2_instrument_name(1, 39) == "Synth Bass 4 (attack)");
  REQUIRE(gm2_instrument_name(2, 62) == "Analog Synth Brass 1");
  REQUIRE(gm2_instrument_name(2, 80) == "Sine Wave");
  REQUIRE(gm2_instrument_name(2, 81) == "Doctor Solo");
  REQUIRE(gm2_instrument_name(2, 102) == "Echo Bell");
  REQUIRE(gm2_instrument_name(1, 103) == "Sci-Fi (FX)");
}

TEST_CASE("GM2 percussion sets re-voice notes and fall back to Standard", "[midi]") {
  // Set names.
  REQUIRE(gm2_drum_set_name(0) == "Standard");
  REQUIRE(gm2_drum_set_name(8) == "Room");
  REQUIRE(gm2_drum_set_name(16) == "Power");
  REQUIRE(gm2_drum_set_name(24) == "Electronic");
  REQUIRE(gm2_drum_set_name(25) == "Analog");
  REQUIRE(gm2_drum_set_name(32) == "Jazz");
  REQUIRE(gm2_drum_set_name(40) == "Brush");
  REQUIRE(gm2_drum_set_name(48) == "Orchestra");
  REQUIRE(gm2_drum_set_name(56) == "SFX");
  REQUIRE(gm2_drum_set_name(7).empty());

  // The Standard set equals the GM drum map for every assigned note.
  for (int n = 35; n <= 81; ++n) {
    REQUIRE(gm2_drum_name(0, static_cast<uint8_t>(n)) == gm_drum_name(static_cast<uint8_t>(n)));
  }

  // Representative re-voicings.
  REQUIRE(gm2_drum_name(8, 41) == "Room Low Tom 2");
  REQUIRE(gm2_drum_name(16, 36) == "Power Kick Drum");
  REQUIRE(gm2_drum_name(24, 36) == "Electric Bass Drum");
  REQUIRE(gm2_drum_name(25, 42) == "Analog Closed Hi-Hat 1");
  REQUIRE(gm2_drum_name(40, 40) == "Brush Swirl");
  REQUIRE(gm2_drum_name(48, 38) == "Concert Snare Drum");

  // A note a set does not re-voice falls back to the Standard GM name.
  REQUIRE(gm2_drum_name(8, 42) == gm_drum_name(42));
  // Unknown bank LSB behaves as Standard.
  REQUIRE(gm2_drum_name(99, 38) == gm_drum_name(38));
  // Out-of-range notes are empty for any set.
  REQUIRE(gm2_drum_name(16, 34).empty());
  REQUIRE(gm2_drum_name(16, 82).empty());
}

TEST_CASE("DrumMapOverride remaps drum notes and is the identity when empty", "[midi]") {
  DrumMapOverride map;
  REQUIRE(map.empty());
  // Empty override is the identity map.
  REQUIRE(map.map_note(38) == 38);

  REQUIRE(map.set_note(38, 40));  // Acoustic Snare -> Electric Snare
  REQUIRE(map.set_note(42, 44));  // Closed Hi Hat -> Pedal Hi-Hat
  REQUIRE_FALSE(map.empty());
  REQUIRE(map.map_note(38) == 40);
  REQUIRE(map.map_note(42) == 44);
  // A note without an entry passes through unchanged.
  REQUIRE(map.map_note(36) == 36);

  // Replacing an existing entry does not grow the table.
  REQUIRE(map.set_note(38, 50));
  REQUIRE(map.map_note(38) == 50);

  // Clearing one entry restores its identity mapping.
  REQUIRE(map.clear_note(42));
  REQUIRE(map.map_note(42) == 42);
  REQUIRE_FALSE(map.clear_note(99));

  // remap_drum_note rewrites a note-on's note number (MIDI 1.0 and 2.0).
  const Ump on1 = make_midi1_note_on(0, 9, 38, 100);
  const Ump remapped1 = remap_drum_note(on1, map);
  REQUIRE(remapped1.note_number() == 50);
  REQUIRE(remapped1.channel() == 9);
  REQUIRE(remapped1.message_type() == on1.message_type());

  const Ump on2 = make_midi2_note_on(0, 9, 38, 0x8000);
  const Ump remapped2 = remap_drum_note(on2, map);
  REQUIRE(remapped2.note_number() == 50);
  REQUIRE(remapped2.message_type() == on2.message_type());

  // A note with no override passes through unchanged.
  const Ump on3 = make_midi1_note_on(0, 9, 36, 100);
  REQUIRE(remap_drum_note(on3, map) == on3);
}

TEST_CASE("GM drum map name and reverse lookup on channel 10", "[midi]") {
  REQUIRE(gm_drum_name(35) == "Acoustic Bass Drum");
  REQUIRE(gm_drum_name(38) == "Acoustic Snare");
  REQUIRE(gm_drum_name(42) == "Closed Hi Hat");
  REQUIRE(gm_drum_name(81) == "Open Triangle");

  REQUIRE(gm_drum_note_for_name("Acoustic Bass Drum") == 35);
  REQUIRE(gm_drum_note_for_name("Open Triangle") == 81);

  // Notes outside the GM percussion range return empty.
  REQUIRE(gm_drum_name(34).empty());
  REQUIRE(gm_drum_name(82).empty());
  REQUIRE(gm_drum_name(0).empty());
  REQUIRE(gm_drum_name(127).empty());
  REQUIRE(gm_drum_note_for_name("No Such Drum") == -1);

  // Round-trip the full assigned range.
  for (int n = 35; n <= 81; ++n) {
    const std::string_view name = gm_drum_name(static_cast<uint8_t>(n));
    REQUIRE_FALSE(name.empty());
    REQUIRE(gm_drum_note_for_name(name) == n);
  }
}

TEST_CASE("CC names cover standard controllers and reverse lookup", "[midi]") {
  REQUIRE(cc_name(0) == "Bank Select (MSB)");
  REQUIRE(cc_name(1) == "Modulation Wheel (MSB)");
  REQUIRE(cc_name(7) == "Channel Volume (MSB)");
  REQUIRE(cc_name(10) == "Pan (MSB)");
  REQUIRE(cc_name(11) == "Expression (MSB)");
  REQUIRE(cc_name(32) == "Bank Select (LSB)");
  REQUIRE(cc_name(64) == "Sustain Pedal");
  REQUIRE(cc_name(123) == "All Notes Off");

  REQUIRE(cc_index_for_name("Bank Select (MSB)") == 0);
  REQUIRE(cc_index_for_name("Pan (MSB)") == 10);
  REQUIRE(cc_index_for_name("All Notes Off") == 123);

  // Undefined controllers return empty and never match in reverse.
  REQUIRE(cc_name(3).empty());
  REQUIRE(cc_name(9).empty());
  REQUIRE(cc_index_for_name("") == -1);
  REQUIRE(cc_index_for_name("Nonexistent CC") == -1);
}

TEST_CASE("Per-note controller names", "[midi]") {
  REQUIRE(per_note_controller_name(1) == "Modulation");
  REQUIRE(per_note_controller_name(7) == "Volume");
  REQUIRE(per_note_controller_name(10) == "Pan");
  REQUIRE(per_note_controller_name(11) == "Expression");
  REQUIRE(per_note_controller_name(0).empty());
  REQUIRE(per_note_controller_name(4).empty());
}

TEST_CASE("Bank + program lowers to three messages and round-trips", "[midi]") {
  ProgramSelection sel;
  sel.bank_msb = 0x79;  // GM2 melodic bank.
  sel.bank_lsb = 1;
  sel.program = 24;  // Ukulele under GM2 variation.
  REQUIRE(sel.bank() == ((0x79u << 7) | 1u));

  const BankProgramMessages msgs = program_to_messages(/*group=*/0, /*channel=*/3, sel);
  REQUIRE(msgs.count == 3);
  REQUIRE(msgs.messages[0].status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(msgs.messages[2].status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));

  // Fold the three messages back into a ProgramState and recover the selection.
  ProgramState state;
  REQUIRE_FALSE(state.observe(msgs.messages[0]));  // bank MSB pending only
  REQUIRE_FALSE(state.observe(msgs.messages[1]));  // bank LSB pending only
  REQUIRE(state.observe(msgs.messages[2]));        // program change commits
  REQUIRE(state.current == sel);

  // Re-applying the identical program does not report a change.
  REQUIRE_FALSE(state.observe(msgs.messages[2]));
}

TEST_CASE("ProgramState ignores unrelated messages", "[midi]") {
  ProgramState state;
  const Ump note = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  REQUIRE_FALSE(state.observe(note));
  REQUIRE_FALSE(state.program_seen);
}

TEST_CASE("ProgramState observes MIDI 2.0 banked program changes", "[midi]") {
  ProgramState state;
  const Ump pc = sonare::midi::make_midi2_program_change(/*group=*/0, /*channel=*/2,
                                                         /*program=*/24, /*bank_msb=*/0x79,
                                                         /*bank_lsb=*/1,
                                                         /*bank_valid=*/true);

  REQUIRE(state.observe(pc));
  REQUIRE(state.program_seen);
  REQUIRE(state.current.bank_msb == 0x79u);
  REQUIRE(state.current.bank_lsb == 1u);
  REQUIRE(state.current.program == 24u);

  // MIDI 2.0 CC bank-select packets update the pending bank for later
  // bank-less program changes.
  REQUIRE_FALSE(state.observe(
      sonare::midi::make_midi2_control_change(0, 2, 0, sonare::midi::scale_cc_7_to_32(0x78))));
  REQUIRE_FALSE(state.observe(
      sonare::midi::make_midi2_control_change(0, 2, 32, sonare::midi::scale_cc_7_to_32(2))));
  const Ump bankless = sonare::midi::make_midi2_program_change(0, 2, 25, 0, 0, false);
  REQUIRE(state.observe(bankless));
  REQUIRE(state.current.bank_msb == 0x78u);
  REQUIRE(state.current.bank_lsb == 2u);
  REQUIRE(state.current.program == 25u);
}

TEST_CASE("DestinationTable add, lookup, remove, and null safety", "[midi]") {
  DestinationTable table;
  REQUIRE(table.size() == 0);

  // Id 0 is the implicit immutable null destination.
  REQUIRE(table.contains(DestinationTable::kNullDestinationId));
  REQUIRE(table.lookup(0).kind == DestinationKind::kNull);
  REQUIRE_FALSE(table.add(0, SoundDestination::HostInstrument(5)));
  REQUIRE_FALSE(table.remove(0));
  REQUIRE(table.size() == 0);

  // Add a host instrument and an external port descriptor.
  const auto host = SoundDestination::HostInstrument(42, "Synth A");
  const auto port = SoundDestination::ExternalPort(7, "Port 1", "Device X", /*group=*/2,
                                                   /*is_midi2=*/true);
  REQUIRE(table.add(100, host));
  REQUIRE(table.add(200, port));
  REQUIRE(table.size() == 2);

  REQUIRE(table.contains(100));
  REQUIRE(table.contains(200));
  REQUIRE(table.lookup(100) == host);
  REQUIRE(table.lookup(100).kind == DestinationKind::kHostInstrument);
  REQUIRE(table.lookup(100).host_instrument.node_id == 42);
  REQUIRE(table.lookup(200) == port);
  REQUIRE(table.lookup(200).external_port.is_midi2);

  // Unknown id resolves safely to a null descriptor without being present.
  REQUIRE_FALSE(table.contains(999));
  REQUIRE(table.lookup(999).kind == DestinationKind::kNull);

  // Replace an existing id.
  REQUIRE(table.add(100, SoundDestination::HostInstrument(43)));
  REQUIRE(table.lookup(100).host_instrument.node_id == 43);
  REQUIRE(table.size() == 2);

  // Sorted id enumeration is deterministic.
  const auto ids = table.ids();
  REQUIRE(ids.size() == 2);
  REQUIRE(ids[0] == 100);
  REQUIRE(ids[1] == 200);

  // Remove.
  REQUIRE(table.remove(100));
  REQUIRE_FALSE(table.contains(100));
  REQUIRE_FALSE(table.remove(100));
  REQUIRE(table.size() == 1);

  table.clear();
  REQUIRE(table.size() == 0);
  // Null destination survives clear.
  REQUIRE(table.contains(0));
}

TEST_CASE("ExternalPortDescriptor is_midi2 gates emission protocol", "[midi]") {
  const auto midi2_port =
      SoundDestination::ExternalPort(1, "P2", "Dev", /*group=*/3, /*is_midi2=*/true);
  const auto midi1_port =
      SoundDestination::ExternalPort(2, "P1", "Dev", /*group=*/5, /*is_midi2=*/false);
  const auto host = SoundDestination::HostInstrument(9);

  REQUIRE(destination_emits_midi2(midi2_port));
  REQUIRE_FALSE(destination_emits_midi2(midi1_port));
  // Non-external destinations keep the internal MIDI 2.0 representation.
  REQUIRE(destination_emits_midi2(host));
  REQUIRE(destination_emits_midi2(SoundDestination::Null()));

  // A MIDI 2.0 event to a MIDI 1.0 port is down-converted (and re-grouped).
  const Ump on2 = make_midi2_note_on(0, 4, 60, 0x8000);
  const Ump to_midi1 = convert_for_destination(on2, midi1_port);
  REQUIRE(to_midi1.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(to_midi1.note_number() == 60);
  REQUIRE(to_midi1.channel() == 4);
  REQUIRE(to_midi1.group == 5);

  // A MIDI 1.0 event to a MIDI 2.0 port is up-converted (and re-grouped).
  const Ump on1 = make_midi1_note_on(0, 4, 60, 100);
  const Ump to_midi2 = convert_for_destination(on1, midi2_port);
  REQUIRE(to_midi2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(to_midi2.note_number() == 60);
  REQUIRE(to_midi2.group == 3);

  // An event already in the target protocol passes through with only the group
  // applied (no lossy re-conversion).
  const Ump already1 = make_midi1_note_on(0, 4, 60, 100);
  const Ump passthrough = convert_for_destination(already1, midi1_port);
  REQUIRE(passthrough.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(passthrough.group == 5);
}
