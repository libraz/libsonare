#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

#include "midi/midi_event.h"
#include "midi/midi_fx.h"
#include "midi/ump.h"

namespace {
using namespace sonare::midi;

MidiEvent note_on(int64_t frame, uint8_t note = 60, uint8_t velocity = 100) {
  return {frame, make_midi1_note_on(0, 0, note, velocity)};
}
}  // namespace

TEST_CASE("MidiFx widens configured pitch offsets before clamping", "[midi_fx_range]") {
  MidiFxChain fx;
  fx.prepare();
  MidiFxBuffer out;

  TransposeConfig transpose{true, std::numeric_limits<int>::max()};
  fx.set_transpose(transpose);
  const MidiEvent source = note_on(10);
  fx.process(&source, 1, &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].ump.note_number() == 127);

  transpose.semitones = std::numeric_limits<int>::min();
  fx.set_transpose(transpose);
  fx.process(&source, 1, &out);
  REQUIRE(out.events[0].ump.note_number() == 0);

  transpose.enabled = false;
  fx.set_transpose(transpose);
  ChordConfig chord;
  chord.enabled = true;
  chord.count = 3;
  chord.intervals = {0, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()};
  fx.set_chord(chord);
  fx.process(&source, 1, &out);
  REQUIRE(out.size == 3);
  REQUIRE(out.events[0].ump.note_number() == 0);
  REQUIRE(out.events[1].ump.note_number() == 60);
  REQUIRE(out.events[2].ump.note_number() == 127);

  chord.enabled = false;
  fx.set_chord(chord);
  ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals = {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()};
  arp.step_frames = std::numeric_limits<int64_t>::max();
  arp.gate_frames = arp.step_frames;
  fx.set_arpeggiator(arp);
  fx.process(&source, 1, &out);
  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].ump.note_number() == 127);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[0].render_frame == 10);
  REQUIRE(out.events[1].ump.note_number() == 0);
  REQUIRE(out.events[1].ump.is_note_off());
  REQUIRE(out.events[2].ump.note_number() == 127);
  REQUIRE(out.events[2].ump.is_note_off());
  REQUIRE(out.events[3].ump.note_number() == 0);
  REQUIRE(out.events[3].ump.is_note_on());
  for (size_t i = 1; i < out.size; ++i) {
    REQUIRE(out.events[i].render_frame == std::numeric_limits<int64_t>::max());
  }

  const MidiEvent lower_source = note_on(std::numeric_limits<int64_t>::min());
  fx.process(&lower_source, 1, &out);
  REQUIRE(out.size == 4);
  REQUIRE(out.events[0].ump.note_number() == 127);
  REQUIRE(out.events[0].ump.is_note_on());
  REQUIRE(out.events[0].render_frame == std::numeric_limits<int64_t>::min());
  REQUIRE(out.events[1].ump.note_number() == 127);
  REQUIRE(out.events[1].ump.is_note_off());
  REQUIRE(out.events[1].render_frame == -1);
  REQUIRE(out.events[2].ump.note_number() == 0);
  REQUIRE(out.events[2].ump.is_note_on());
  REQUIRE(out.events[2].render_frame == -1);
  REQUIRE(out.events[3].ump.note_number() == 0);
  REQUIRE(out.events[3].ump.is_note_off());
  REQUIRE(out.events[3].render_frame == std::numeric_limits<int64_t>::max() - 1);

  // The same widened addition path preserves MIDI 2.0 attributes while
  // saturating its note number into the MIDI note domain.
  const MidiEvent midi2 = {20, make_midi2_note_on(2, 5, 60, 0x4321u, 7, 0x2468u)};
  fx.set_arpeggiator({});
  transpose.enabled = true;
  transpose.semitones = std::numeric_limits<int>::max();
  fx.set_transpose(transpose);
  fx.process(&midi2, 1, &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].ump.note_number() == 127);
  REQUIRE((out.events[0].ump.words[0] & 0xffu) == 7u);
  REQUIRE((out.events[0].ump.words[1] & 0xffffu) == 0x2468u);

  transpose.semitones = std::numeric_limits<int>::min();
  fx.set_transpose(transpose);
  fx.process(&midi2, 1, &out);
  REQUIRE(out.events[0].ump.note_number() == 0);

  transpose.enabled = false;
  fx.set_transpose(transpose);
  chord.enabled = true;
  chord.count = 2;
  chord.intervals = {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()};
  fx.set_arpeggiator({});
  fx.set_chord(chord);
  fx.process(&midi2, 1, &out);
  REQUIRE(out.size == 2);
  REQUIRE(out.events[0].ump.note_number() == 0);
  REQUIRE(out.events[1].ump.note_number() == 127);
  REQUIRE((out.events[0].ump.words[0] & 0xffu) == 7u);
  REQUIRE((out.events[0].ump.words[1] & 0xffffu) == 0x2468u);
  REQUIRE((out.events[1].ump.words[0] & 0xffu) == 7u);
  REQUIRE((out.events[1].ump.words[1] & 0xffffu) == 0x2468u);

  chord.enabled = false;
  fx.set_chord(chord);
  ArpeggiatorConfig midi2_arp;
  midi2_arp.enabled = true;
  midi2_arp.steps = 2;
  midi2_arp.intervals = {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()};
  midi2_arp.step_frames = 1;
  midi2_arp.gate_frames = 1;
  fx.set_arpeggiator(midi2_arp);
  fx.process(&midi2, 1, &out);
  REQUIRE(out.size == 4);
  size_t arp_note_ons = 0;
  for (size_t i = 0; i < out.size; ++i) {
    const auto& ump = out.events[i].ump;
    if (!ump.is_note_on()) continue;
    REQUIRE(ump.note_number() == (arp_note_ons == 0 ? 127 : 0));
    REQUIRE((ump.words[0] & 0xffu) == 7u);
    REQUIRE((ump.words[1] & 0xffffu) == 0x2468u);
    ++arp_note_ons;
  }
  REQUIRE(arp_note_ons == 2);
}

TEST_CASE("MidiFx saturates extreme velocity curves before integer narrowing", "[midi_fx_range]") {
  MidiFxChain fx;
  fx.prepare();
  MidiFxBuffer out;
  VelocityCurveConfig curve;
  curve.enabled = true;
  curve.scale = std::numeric_limits<float>::max();
  fx.set_velocity_curve(curve);
  const MidiEvent source[] = {note_on(0), {1, make_midi2_note_on(0, 0, 60, 32768)}};
  fx.process(source, 2, &out);
  REQUIRE(out.size == 2);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 127);
  REQUIRE(static_cast<uint16_t>(out.events[1].ump.words[1] >> 16u) == 65535);

  curve.scale = std::numeric_limits<float>::max();
  curve.offset = -std::numeric_limits<float>::max();
  fx.set_velocity_curve(curve);
  fx.process(source, 2, &out);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 127);
  REQUIRE(static_cast<uint16_t>(out.events[1].ump.words[1] >> 16u) == 65535);

  curve.scale = -std::numeric_limits<float>::max();
  curve.offset = std::numeric_limits<float>::max();
  fx.set_velocity_curve(curve);
  fx.process(source, 2, &out);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 1);
  REQUIRE(static_cast<uint16_t>(out.events[1].ump.words[1] >> 16u) == 1);

  curve.scale = std::numeric_limits<float>::infinity();
  curve.offset = -std::numeric_limits<float>::infinity();
  fx.set_velocity_curve(curve);
  fx.process(source, 2, &out);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 1);
  REQUIRE(static_cast<uint16_t>(out.events[1].ump.words[1] >> 16u) == 1);

  curve.scale = std::numeric_limits<float>::quiet_NaN();
  curve.offset = 0.0f;
  fx.set_velocity_curve(curve);
  fx.process(source, 2, &out);
  REQUIRE(midi1_velocity7(out.events[0].ump) == 1);
  REQUIRE(static_cast<uint16_t>(out.events[1].ump.words[1] >> 16u) == 1);
}

TEST_CASE("MidiFx quantize is bounded at int64 timeline endpoints", "[midi_fx_range]") {
  MidiFxChain fx;
  fx.prepare();
  QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 1;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  MidiFxBuffer out;

  for (const int64_t frame :
       {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
    const MidiEvent source = note_on(frame);
    fx.process(&source, 1, &out);
    REQUIRE(out.size == 1);
    REQUIRE(out.events[0].render_frame == frame);
  }

  quantize.grid_frames = std::numeric_limits<int64_t>::max();
  fx.set_quantize(quantize);
  const MidiEvent zero = note_on(0);
  fx.process(&zero, 1, &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].render_frame == 0);

  quantize.grid_frames = 100;
  quantize.strength = 0.0f;
  fx.set_quantize(quantize);
  const MidiEvent between = note_on(37);
  fx.process(&between, 1, &out);
  REQUIRE(out.events[0].render_frame == 37);

  // Full-range grid origins can make the relative subtraction saturate. The
  // midpoint must still interpolate the original and snapped frames safely.
  quantize.grid_frames = std::numeric_limits<int64_t>::max();
  quantize.strength = 0.5f;
  fx.set_quantize(quantize);
  const MidiEvent lower = note_on(std::numeric_limits<int64_t>::min());
  fx.process_chunk(&lower, 1, 0, &out, false, 0, std::numeric_limits<int64_t>::max());
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].render_frame == std::numeric_limits<int64_t>::min() / 2);

  const MidiEvent upper = note_on(std::numeric_limits<int64_t>::max());
  fx.process_chunk(&upper, 1, 0, &out, false, 0, std::numeric_limits<int64_t>::min());
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].render_frame == std::numeric_limits<int64_t>::max() / 2);

  quantize.grid_frames = std::numeric_limits<int64_t>::max();
  quantize.strength = 1.0f;
  quantize.swing = 1.0f;
  quantize.groove_steps = 3;
  quantize.groove_offsets = {1.0f, -1.0f, 0.5f};
  fx.set_quantize(quantize);
  for (const int64_t frame :
       {std::numeric_limits<int64_t>::min(), int64_t{0}, std::numeric_limits<int64_t>::max()}) {
    const MidiEvent extreme = note_on(frame);
    fx.process(&extreme, 1, &out);
    REQUIRE(out.size == 1);
  }
}

TEST_CASE("MidiFx humanize remains deterministic with maximal accepted ranges", "[midi_fx_range]") {
  HumanizeConfig humanize;
  humanize.enabled = true;
  humanize.timing_frames = std::numeric_limits<int64_t>::max();
  humanize.velocity_amount = std::numeric_limits<int>::max();
  const MidiEvent source[] = {note_on(1), {2, make_midi2_note_on(0, 0, 61, 32768)}};

  for (uint32_t seed : {0u, 1u, 0x12345678u, std::numeric_limits<uint32_t>::max()}) {
    humanize.seed = seed;
    MidiFxChain first;
    MidiFxChain second;
    first.prepare();
    second.prepare();
    first.set_humanize(humanize);
    second.set_humanize(humanize);
    MidiFxBuffer a, b;
    first.process(source, 2, &a);
    second.process(source, 2, &b);
    REQUIRE(a.size == 2);
    REQUIRE(b.size == 2);
    for (size_t i = 0; i < a.size; ++i) {
      REQUIRE(a.events[i].render_frame >= 0);
      REQUIRE(a.events[i].render_frame == b.events[i].render_frame);
      REQUIRE(a.events[i].ump.words[0] == b.events[i].ump.words[0]);
      REQUIRE(a.events[i].ump.words[1] == b.events[i].ump.words[1]);
      if (a.events[i].ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
        REQUIRE(static_cast<uint16_t>(a.events[i].ump.words[1] >> 16u) > 0u);
      } else {
        REQUIRE(midi1_velocity7(a.events[i].ump) > 0u);
      }
    }
  }
}

TEST_CASE("MidiFx preserves the historical humanize stream and chunk ordinals", "[midi_fx_range]") {
  HumanizeConfig humanize;
  humanize.enabled = true;
  humanize.seed = 12345;
  humanize.timing_frames = 20;
  humanize.velocity_amount = 15;
  const MidiEvent source[] = {note_on(1000, 60, 90), note_on(2000, 61, 90)};

  MidiFxChain whole_fx;
  whole_fx.prepare();
  whole_fx.set_humanize(humanize);
  MidiFxBuffer whole;
  whole_fx.process(source, 2, &whole);
  REQUIRE(whole.size == 2);
  // Literal SplitMix64/modulo reference for seed 12345, event ordinal zero:
  // timing draw 24 -> +4 frames, velocity draw 5 -> -10 velocity units.
  REQUIRE(whole.events[0].render_frame == 1004);
  REQUIRE(midi1_velocity7(whole.events[0].ump) == 80);

  MidiFxChain chunked_fx;
  chunked_fx.prepare();
  chunked_fx.set_humanize(humanize);
  MidiFxBuffer first, second;
  chunked_fx.process_chunk(source, 1, 0, &first);
  chunked_fx.process_chunk(source + 1, 1, 1, &second);
  REQUIRE(first.size == 1);
  REQUIRE(second.size == 1);
  REQUIRE(first.events[0].render_frame == whole.events[0].render_frame);
  REQUIRE(first.events[0].ump.words[0] == whole.events[0].ump.words[0]);
  REQUIRE(second.events[0].render_frame == whole.events[1].render_frame);
  REQUIRE(second.events[0].ump.words[0] == whole.events[1].ump.words[0]);
}
