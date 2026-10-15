/// @file midi_fx_nan_config_test.cpp
/// @brief MIDI FX quantize: non-finite strength / swing are treated as 0.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

#include "midi/midi_event.h"
#include "midi/midi_fx.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiFxBuffer;
using sonare::midi::MidiFxChain;
using sonare::midi::QuantizeConfig;

MidiEvent note_on(int64_t frame, uint8_t note, uint8_t vel) {
  return {frame, sonare::midi::make_midi1_note_on(0, 0, note, vel)};
}

int64_t quantized_frame(const QuantizeConfig& q, int64_t frame) {
  MidiFxChain fx;
  fx.prepare();
  fx.set_quantize(q);
  std::vector<MidiEvent> in = {note_on(frame, 60, 100)};
  MidiFxBuffer out;
  fx.process(in.data(), in.size(), &out);
  REQUIRE(out.size == 1);
  return out.events[0].render_frame;
}

}  // namespace

TEST_CASE("MidiFx quantize treats a non-finite strength as no snap", "[midi]") {
  QuantizeConfig q;
  q.enabled = true;
  q.grid_frames = 100;
  q.strength = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(quantized_frame(q, 37) == 37);

  q.strength = std::numeric_limits<float>::infinity();
  REQUIRE(quantized_frame(q, 37) == 37);
}

TEST_CASE("MidiFx quantize treats a non-finite swing as a straight grid", "[midi]") {
  QuantizeConfig straight;
  straight.enabled = true;
  straight.grid_frames = 100;
  straight.strength = 1.0f;
  straight.swing = 0.0f;

  QuantizeConfig nan_swing = straight;
  nan_swing.swing = std::numeric_limits<float>::quiet_NaN();

  for (int64_t frame : {int64_t{0}, int64_t{37}, int64_t{100}, int64_t{140}, int64_t{151}}) {
    REQUIRE(quantized_frame(nan_swing, frame) == quantized_frame(straight, frame));
  }
}
