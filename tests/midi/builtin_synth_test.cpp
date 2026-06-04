/// @file builtin_synth_test.cpp
/// @brief MIDI core: the minimal built-in synth's channel-mode handling —
///        CC#123 (All Notes Off) releases voices and CC#120 (All Sound Off)
///        silences them immediately.

#include "midi/builtin_synth.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::MidiEvent;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

// Renders `num_samples` mono frames and returns the peak absolute amplitude.
float render_peak(BuiltinSynth* synth, int num_samples) {
  std::vector<float> buffer(static_cast<size_t>(num_samples), 0.0f);
  float* channels[1] = {buffer.data()};
  synth->process(channels, 1, num_samples);
  float peak = 0.0f;
  for (float s : buffer) peak = std::max(peak, std::fabs(s));
  return peak;
}

}  // namespace

TEST_CASE("BuiltinSynth CC#123 (All Notes Off) releases sounding voices", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;  // Short release so the tail decays quickly.
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);  // The note is sounding.

  // A bare CC#123 with no accompanying note-off must still silence the synth.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  // After the (short) release tail, output must reach silence.
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth ignores events before prepare without poisoning later render",
          "[midi][synth]") {
  BuiltinSynth synth({});

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 64)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));

  synth.prepare(48000.0, 128);
  REQUIRE(render_peak(&synth, 256) == 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
}

TEST_CASE("BuiltinSynth CC#120 (All Sound Off) silences voices immediately", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 2000.0f;  // Long release: only an immediate kill silences it fast.
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  // No release tail — the very next block is already silent.
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth CC#64 holds released notes until pedal is lifted", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth Reset All Controllers lifts sustain pedal", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth All Notes Off only affects the addressed channel", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));

  // All Sound Off on channel 0 must leave channel 1's note sounding.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
}
