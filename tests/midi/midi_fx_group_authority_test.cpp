#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "midi/midi_event.h"
#include "midi/midi_fx.h"
#include "midi/ump.h"

TEST_CASE("MidiFx uses the UMP word group when reshaping note packets", "[midi_fx_group]") {
  sonare::midi::MidiFxChain fx;
  fx.prepare();
  sonare::midi::TransposeConfig transpose;
  transpose.enabled = true;
  transpose.semitones = 2;
  fx.set_transpose(transpose);
  sonare::midi::VelocityCurveConfig velocity;
  velocity.enabled = true;
  velocity.scale = 1.0f;
  velocity.offset = 1.0f;
  fx.set_velocity_curve(velocity);

  sonare::midi::MidiEvent source{120,
                                 sonare::midi::make_midi2_note_on(3, 4, 60, 0x4000u, 2, 0x1234u)};
  source.ump.group = 0;  // Bindings may leave this cached field stale.
  sonare::midi::MidiFxBuffer out;
  fx.process(&source, 1, &out);

  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].ump.note_number() == 62);
  REQUIRE(out.events[0].ump.group == 3);
  REQUIRE(sonare::midi::ump_group_from_word0(out.events[0].ump.words[0]) == 3);
  REQUIRE(out.events[0].ump.channel() == 4);
  REQUIRE((out.events[0].ump.words[0] & 0xffu) == 2u);
  REQUIRE((out.events[0].ump.words[1] & 0xffffu) == 0x1234u);
  REQUIRE(source.ump.group == 0);
}

TEST_CASE("MidiFx pairs shaped note timing by authoritative wire group", "[midi_fx_group]") {
  sonare::midi::MidiFxChain fx;
  fx.prepare();
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);

  sonare::midi::MidiEvent input[] = {
      {60, sonare::midi::make_midi1_note_on(3, 0, 60, 100)},
      {70, sonare::midi::make_midi1_note_on(4, 0, 60, 100)},
      {90, sonare::midi::make_midi1_note_off(3, 0, 60, 0)},
      {110, sonare::midi::make_midi1_note_off(4, 0, 60, 0)},
  };
  // Make the stale sideband group on each off collide with the other group's
  // note-on, so both output metadata and the pairing key must use word0.
  input[0].ump.group = 0;
  input[1].ump.group = 3;
  input[2].ump.group = 4;
  input[3].ump.group = 0;
  sonare::midi::MidiFxBuffer out;
  fx.process(input, 4, &out);
  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].ump.group == 3);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[0].render_frame == 100);
  REQUIRE(out.events[1].ump.group == 4);
  REQUIRE(out.events[1].ump.is_note_on());
  REQUIRE(out.events[1].render_frame == 100);
  REQUIRE(out.events[2].ump.group == 3);
  REQUIRE(out.events[2].ump.is_note_off());
  REQUIRE(out.events[2].render_frame == 130);
  REQUIRE(out.events[3].ump.group == 4);
  REQUIRE(out.events[3].ump.is_note_off());
  REQUIRE(out.events[3].render_frame == 140);
}

TEST_CASE("MidiFx clears pending note timing by authoritative wire group", "[midi_fx_group]") {
  sonare::midi::MidiFxChain fx;
  fx.prepare();
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);

  sonare::midi::MidiEvent note_on{60, sonare::midi::make_midi1_note_on(3, 0, 60, 100)};
  note_on.ump.group = 0;
  sonare::midi::MidiFxBuffer out;
  fx.process(&note_on, 1, &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].render_frame == 100);

  sonare::midi::MidiEvent queued = out.events[0];
  queued.ump.group = 0;
  fx.clear_pending_note_tracking(queued);

  sonare::midi::MidiEvent note_off{90, sonare::midi::make_midi1_note_off(3, 0, 60, 0)};
  note_off.ump.group = 0;
  fx.process(&note_off, 1, &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].render_frame == 90);
}

TEST_CASE("MidiFx passes SysEx event metadata through unchanged", "[midi_fx_group]") {
  sonare::midi::MidiFxChain fx;
  fx.prepare();
  sonare::midi::TransposeConfig transpose;
  transpose.enabled = true;
  transpose.semitones = 12;
  fx.set_transpose(transpose);

  const uint8_t payload[] = {0x7du, 0x01u, 0x02u};
  sonare::midi::MidiEvent source{
      123, sonare::midi::make_sysex_handle(3, 0x12345678u), 42, payload, sizeof(payload), nullptr};
  source.ump.group = 0;
  sonare::midi::MidiFxBuffer out;
  fx.process(&source, 1, &out);

  REQUIRE(out.size == 1);
  REQUIRE(out.events[0] == source);
}
