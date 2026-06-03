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
using sonare::midi::DestinationKind;
using sonare::midi::DestinationTable;
using sonare::midi::gm2_instrument_name;
using sonare::midi::gm_drum_name;
using sonare::midi::gm_drum_note_for_name;
using sonare::midi::gm_family_first_program;
using sonare::midi::gm_family_name;
using sonare::midi::gm_instrument_name;
using sonare::midi::gm_program_for_name;
using sonare::midi::per_note_controller_name;
using sonare::midi::program_to_messages;
using sonare::midi::ProgramSelection;
using sonare::midi::ProgramState;
using sonare::midi::SoundDestination;
using sonare::midi::Ump;
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
