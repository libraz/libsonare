/// @file midi_fx_test.cpp
/// @brief MIDI FX: per-stage transform correctness, deterministic humanize,
///        fan-out (chord / arpeggiator), overflow telemetry, and an
///        allocation-0 assertion on the audio process() path.
///
/// The allocation-0 check reuses the SAME mechanism as
/// tests/mixing/no_alloc_test (the global operator new / delete overrides defined
/// there feed sonare::test counters; AllocationGuard arms/disarms them). We do
/// NOT redefine the overrides here.

#include "midi/midi_fx.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"

namespace {

using sonare::midi::ArpeggiatorConfig;
using sonare::midi::ChordConfig;
using sonare::midi::HumanizeConfig;
using sonare::midi::midi1_velocity7;
using sonare::midi::MidiEvent;
using sonare::midi::MidiFxBuffer;
using sonare::midi::MidiFxChain;
using sonare::midi::QuantizeConfig;
using sonare::midi::TransposeConfig;
using sonare::midi::VelocityCurveConfig;
using sonare::test::AllocationGuard;

MidiEvent note_on(int64_t frame, uint8_t note, uint8_t vel) {
  return {frame, sonare::midi::make_midi1_note_on(0, 0, note, vel)};
}
MidiEvent note_off(int64_t frame, uint8_t note) {
  return {frame, sonare::midi::make_midi1_note_off(0, 0, note, 0)};
}

}  // namespace

TEST_CASE("MidiFx transpose shifts note numbers and clamps", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  TransposeConfig t;
  t.enabled = true;
  t.semitones = 7;
  fx.set_transpose(t);

  std::vector<MidiEvent> in = {note_on(0, 60, 100), note_off(48, 60)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == 2);
  REQUIRE(out.events[0].ump.note_number() == 67);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[1].ump.note_number() == 67);
  REQUIRE(out.events[1].ump.is_note_off());

  // Clamp at the top: 125 + 7 -> 127.
  std::vector<MidiEvent> high = {note_on(0, 125, 90)};
  fx.process(high.data(), high.size(), &out);
  REQUIRE(out.events[0].ump.note_number() == 127);
}

TEST_CASE("MidiFx quantize snaps render frames to grid", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 100;
  q.strength = 1.0f;
  fx.set_quantize(q);

  std::vector<MidiEvent> in = {note_on(0, 60, 100), note_on(140, 61, 100), note_on(149, 62, 100),
                               note_on(151, 63, 100)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].render_frame == 0);
  REQUIRE(out.events[1].render_frame == 100);  // 140 -> nearest 100
  REQUIRE(out.events[2].render_frame == 100);  // 149 -> nearest 100
  REQUIRE(out.events[3].render_frame == 200);  // 151 -> nearest 200

  // Half strength moves only halfway.
  q.strength = 0.5f;
  fx.set_quantize(q);
  std::vector<MidiEvent> half = {note_on(140, 60, 100)};  // snap target 100, delta -40
  fx.process(half.data(), half.size(), &out);
  REQUIRE(out.events[0].render_frame == 120);  // 140 + (-40 * 0.5)
}

TEST_CASE("MidiFx quantize can swing odd grid lines", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 100;
  q.strength = 1.0f;
  q.swing = 1.0f;
  fx.set_quantize(q);

  std::vector<MidiEvent> in = {
      note_on(130, 60, 100),  // nearest swung line 1: 100 + 50 = 150
      note_on(205, 61, 100),  // even line 2 remains 200
      note_on(310, 62, 100),  // swung line 3: 300 + 50 = 350
  };
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == 3);
  REQUIRE(out.events[0].render_frame == 150);
  REQUIRE(out.events[1].render_frame == 200);
  REQUIRE(out.events[2].render_frame == 350);

  q.strength = 0.5f;
  fx.set_quantize(q);
  std::vector<MidiEvent> half = {note_on(130, 60, 100)};
  fx.process(half.data(), half.size(), &out);
  REQUIRE(out.events[0].render_frame == 140);  // halfway from 130 to swung 150
}

TEST_CASE("MidiFx quantize applies repeating groove template offsets", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 100;
  q.strength = 1.0f;
  q.groove_steps = 4;
  q.groove_offsets = {0.0f, 0.10f, -0.10f, 0.25f};
  fx.set_quantize(q);

  std::vector<MidiEvent> in = {
      note_on(112, 60, 100),  // line 1: 100 + 10
      note_on(188, 61, 100),  // line 2: 200 - 10
      note_on(330, 62, 100),  // line 3: 300 + 25
      note_on(405, 63, 100),  // line 4 wraps to template index 0
  };
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].render_frame == 110);
  REQUIRE(out.events[1].render_frame == 190);
  REQUIRE(out.events[2].render_frame == 325);
  REQUIRE(out.events[3].render_frame == 400);
}

TEST_CASE("MidiFx velocity curve maps velocity", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  VelocityCurveConfig v;
  v.enabled = true;
  v.scale = 0.5f;
  v.offset = 0.0f;
  v.gamma = 1.0f;
  fx.set_velocity_curve(v);

  std::vector<MidiEvent> in = {note_on(0, 60, 100)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);
  REQUIRE(out.size == 1);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 50);

  // A note-on never falls below velocity 1 even with zero scale.
  v.scale = 0.0f;
  v.offset = 0.0f;
  fx.set_velocity_curve(v);
  fx.process(in.data(), in.size(), &out);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 1);
}

TEST_CASE("MidiFx chord expands a single note into multiple notes", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  ChordConfig c;
  c.enabled = true;
  c.count = 3;
  c.intervals = {0, 4, 7};  // major triad
  fx.set_chord(c);

  std::vector<MidiEvent> in = {note_on(0, 60, 100), note_off(48, 60)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == 6);  // 3 note-ons + 3 note-offs
  REQUIRE(out.events[0].ump.note_number() == 60);
  REQUIRE(out.events[1].ump.note_number() == 64);
  REQUIRE(out.events[2].ump.note_number() == 67);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[3].ump.note_number() == 60);
  REQUIRE(out.events[3].ump.is_note_off());
}

TEST_CASE("MidiFx handles MIDI 2.0 note events without down-converting", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  TransposeConfig t;
  t.enabled = true;
  t.semitones = 12;
  fx.set_transpose(t);
  VelocityCurveConfig v;
  v.enabled = true;
  v.scale = 0.5f;
  v.gamma = 1.0f;
  fx.set_velocity_curve(v);
  ChordConfig c;
  c.enabled = true;
  c.count = 2;
  c.intervals = {0, 7};
  fx.set_chord(c);

  const MidiEvent in[] = {{10, sonare::midi::make_midi2_note_on(1, 3, 60, 0x8000u)},
                          {20, sonare::midi::make_midi2_note_off(1, 3, 60, 0)}};
  MidiFxBuffer out;
  fx.process(in, 2, &out);

  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].ump.message_type() == sonare::midi::UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(out.events[0].ump.channel() == 3);
  REQUIRE(out.events[0].ump.note_number() == 72);
  REQUIRE(static_cast<uint16_t>(out.events[0].ump.words[1] >> 16u) ==
          sonare::midi::scale_velocity_7_to_16(32));
  REQUIRE(out.events[1].ump.note_number() == 79);
  REQUIRE(out.events[2].ump.message_type() == sonare::midi::UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(out.events[2].ump.is_note_off());
  REQUIRE(out.events[2].ump.note_number() == 72);
  REQUIRE(out.events[3].ump.note_number() == 79);
}

TEST_CASE("MidiFx maps MIDI 2.0 per-note controller notes without changing controller value",
          "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  TransposeConfig t;
  t.enabled = true;
  t.semitones = 12;
  fx.set_transpose(t);
  ChordConfig c;
  c.enabled = true;
  c.count = 2;
  c.intervals = {0, 7};
  fx.set_chord(c);

  const MidiEvent in[] = {
      {10, sonare::midi::make_midi2_per_note_controller(1, 3, 60, 1, 0x12345678u)},
      {20, sonare::midi::make_midi2_assignable_per_note_controller(1, 3, 61, 9, 0x87654321u)},
  };
  MidiFxBuffer out;
  fx.process(in, 2, &out);

  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kRegisteredPerNoteController));
  REQUIRE(out.events[0].ump.channel() == 3);
  REQUIRE(out.events[0].ump.note_number() == 72);
  REQUIRE(static_cast<uint8_t>(out.events[0].ump.words[0] & 0xFFu) == 1);
  REQUIRE(out.events[0].ump.words[1] == 0x12345678u);
  REQUIRE(out.events[1].ump.note_number() == 79);
  REQUIRE(out.events[1].ump.words[1] == 0x12345678u);

  REQUIRE(out.events[2].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kAssignablePerNoteController));
  REQUIRE(out.events[2].ump.note_number() == 73);
  REQUIRE(static_cast<uint8_t>(out.events[2].ump.words[0] & 0xFFu) == 9);
  REQUIRE(out.events[2].ump.words[1] == 0x87654321u);
  REQUIRE(out.events[3].ump.note_number() == 80);
  REQUIRE(out.events[3].ump.words[1] == 0x87654321u);
}

TEST_CASE("MidiFx preserves MIDI 2.0 channel controller forms while time shaping", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  TransposeConfig t;
  t.enabled = true;
  t.semitones = 12;
  fx.set_transpose(t);
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 100;
  q.strength = 1.0f;
  fx.set_quantize(q);

  const MidiEvent in[] = {
      {149, sonare::midi::make_midi2_registered_controller(1, 3, 2, 10, 0x10203040u)},
      {151, sonare::midi::make_midi2_assignable_controller(1, 3, 5, 12, 0xA0B0C0D0u)},
  };
  MidiFxBuffer out;
  fx.process(in, 2, &out);

  REQUIRE(out.size == 2);
  REQUIRE(out.events[0].render_frame == 100);
  REQUIRE(out.events[0].ump == in[0].ump);
  REQUIRE(out.events[1].render_frame == 200);
  REQUIRE(out.events[1].ump == in[1].ump);
}

TEST_CASE("MidiFx arpeggiator expands a held note into gated steps", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  ArpeggiatorConfig a;
  a.enabled = true;
  a.steps = 3;
  a.intervals = {0, 4, 7};
  a.step_frames = 100;
  a.gate_frames = 50;
  fx.set_arpeggiator(a);

  std::vector<MidiEvent> in = {note_on(0, 60, 100), note_off(400, 60)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  // 3 steps -> 3 on/off pairs = 6 events; the source note-off is consumed.
  REQUIRE(out.size == 6);
  REQUIRE(out.events[0].render_frame == 0);
  REQUIRE(out.events[0].ump.note_number() == 60);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[1].render_frame == 50);
  REQUIRE(out.events[1].ump.is_note_off());
  REQUIRE(out.events[2].render_frame == 100);
  REQUIRE(out.events[2].ump.note_number() == 64);
  REQUIRE(out.events[4].render_frame == 200);
  REQUIRE(out.events[4].ump.note_number() == 67);
}

TEST_CASE("MidiFx humanize is reproducible for a fixed seed and differs across seeds", "[midi]") {
  HumanizeConfig h;
  h.enabled = true;
  h.timing_frames = 20;
  h.velocity_amount = 15;

  std::vector<MidiEvent> in;
  for (int i = 0; i < 16; ++i) {
    in.push_back(note_on(static_cast<int64_t>(i) * 100, static_cast<uint8_t>(60 + i), 90));
  }

  auto run = [&](uint32_t seed) {
    MidiFxChain fx;
    fx.prepare();
    h.seed = seed;
    fx.set_humanize(h);
    MidiFxBuffer out;
    fx.process(in.data(), in.size(), &out);
    std::vector<MidiEvent> result(out.events.begin(), out.events.begin() + out.size);
    return result;
  };

  const auto a1 = run(12345);
  const auto a2 = run(12345);
  const auto b = run(99999);

  REQUIRE(a1.size() == in.size());
  // Same seed -> identical output.
  REQUIRE(a1 == a2);
  // Different seed -> different output (at least one event differs).
  bool differs = false;
  for (size_t i = 0; i < a1.size(); ++i) {
    if (a1[i] != b[i]) {
      differs = true;
      break;
    }
  }
  REQUIRE(differs);

  // Jitter stays within configured bounds.
  for (size_t i = 0; i < a1.size(); ++i) {
    const int64_t nominal = static_cast<int64_t>(i) * 100;
    REQUIRE(a1[i].render_frame >= nominal - 20);
    REQUIRE(a1[i].render_frame <= nominal + 20);
    const int vel = midi1_velocity7(a1[i].ump);
    REQUIRE(vel >= 90 - 15);
    REQUIRE(vel <= 90 + 15);
  }
}

TEST_CASE("MidiFx overflow bumps telemetry and never grows beyond capacity", "[midi]") {
  MidiFxChain fx;
  fx.prepare();
  // Chord of the max size multiplies the event count; drive far past capacity.
  ChordConfig c;
  c.enabled = true;
  c.count = ChordConfig::kMaxChordNotes;
  for (size_t i = 0; i < ChordConfig::kMaxChordNotes; ++i) {
    c.intervals[i] = static_cast<int>(i);
  }
  fx.set_chord(c);

  // Each input note-on -> kMaxChordNotes outputs. Feed enough to exceed kCapacity.
  std::vector<MidiEvent> in;
  const size_t notes_needed = (MidiFxBuffer::kCapacity / ChordConfig::kMaxChordNotes) + 16;
  for (size_t i = 0; i < notes_needed; ++i) {
    in.push_back(note_on(static_cast<int64_t>(i), 60, 100));
  }

  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);

  REQUIRE(out.size == MidiFxBuffer::kCapacity);  // never grew
  REQUIRE(fx.overflow_count() > 0);

  const size_t expected_total = in.size() * ChordConfig::kMaxChordNotes;
  REQUIRE(fx.overflow_count() == expected_total - MidiFxBuffer::kCapacity);
}

TEST_CASE("MidiFx process performs no heap allocation after prepare", "[midi][rt]") {
  MidiFxChain fx;
  fx.prepare();
  TransposeConfig t;
  t.enabled = true;
  t.semitones = 5;
  fx.set_transpose(t);
  VelocityCurveConfig v;
  v.enabled = true;
  v.scale = 1.1f;
  v.gamma = 0.9f;
  fx.set_velocity_curve(v);
  ChordConfig c;
  c.enabled = true;
  c.count = 3;
  c.intervals = {0, 3, 7};
  fx.set_chord(c);
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 64;
  q.strength = 0.8f;
  fx.set_quantize(q);
  HumanizeConfig h;
  h.enabled = true;
  h.seed = 777;
  h.timing_frames = 8;
  h.velocity_amount = 6;
  fx.set_humanize(h);

  std::vector<MidiEvent> in;
  for (int i = 0; i < 32; ++i) {
    const uint8_t note = static_cast<uint8_t>(48 + (i % 12));
    if (i % 2 == 0) {
      in.push_back(note_on(static_cast<int64_t>(i) * 16, note, 96));
    } else {
      in.push_back(note_off(static_cast<int64_t>(i) * 16, note));
    }
  }

  MidiFxBuffer out;
  // Warm-up run before arming the guard.
  fx.process(in.data(), in.size(), &out);

  AllocationGuard guard;
  fx.process(in.data(), in.size(), &out);
  REQUIRE(guard.count() == 0);
}
