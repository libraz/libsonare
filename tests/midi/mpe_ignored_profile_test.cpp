#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "midi/controller_profile.h"
#include "midi/synth/native_synth.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {
using namespace sonare::midi;
using namespace sonare::midi::synth;

std::vector<float> render_profile_input(bool zoned, bool deliver, bool poly_pressure,
                                        uint8_t controller, uint8_t channel, bool midi2) {
  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kReed;
  cfg.patch.amp_env.sustain = 1.0f;
  NativeSynth synth(cfg);
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {poly_pressure ? ControllerInput::kPolyPressure : ControllerInput::kControlChange, controller,
       ControllerAxis::kBrightness, 0.0f, 1.0f}));
  REQUIRE(synth.set_controller_profile(profile));
  synth.prepare(48000.0, 256);
  const auto send = [&](const Ump& ump) { synth.on_event(0, sonare::test::event(ump)); };
  if (zoned) {
    send(make_midi1_control_change(0, 0, 101, 0));
    send(make_midi1_control_change(0, 0, 100, 6));
    send(make_midi1_control_change(0, 0, 6, 7));
  }
  send(make_midi1_note_on(0, channel, 60, 100));
  sonare::test::render_left(synth, 4096);
  if (deliver) {
    if (midi2) {
      send(poly_pressure ? make_midi2_poly_pressure(0, channel, 60, 0xFFFFFFFFu)
                         : make_midi2_control_change(0, channel, controller, 0xFFFFFFFFu));
    } else {
      send(poly_pressure ? make_midi1_poly_pressure(0, channel, 60, 127)
                         : make_midi1_control_change(0, channel, controller, 127));
    }
  }
  return sonare::test::render_left(synth, 8192);
}
}  // namespace

TEST_CASE("MPE member bend freezes the modulation matrix source after Note Off",
          "[midi][mpe][wind]") {
  const auto render = [](bool matrix, bool update) {
    NativeSynthConfig cfg;
    cfg.patch.mode = SynthEngineMode::kReed;
    cfg.patch.amp_env.sustain = 1.0f;
    if (matrix) {
      cfg.patch.mod_matrix.routes[0] = {ModSource::kPitchBend,
                                        ModDestination::kExcitationBrightness, 1.0f};
    }
    NativeSynth synth(cfg);
    synth.prepare(48000.0, 256);
    const auto send = [&](const Ump& ump) { synth.on_event(0, sonare::test::event(ump)); };
    send(make_midi1_control_change(0, 0, 101, 0));
    send(make_midi1_control_change(0, 0, 100, 6));
    send(make_midi1_control_change(0, 0, 6, 7));
    send(make_midi1_note_on(0, 2, 60, 100));
    sonare::test::render_left(synth, 4096);
    send(make_midi1_note_off(0, 2, 60, 0));
    sonare::test::render_left(synth, 1024);
    if (update) send(make_midi1_pitch_bend(0, 2, 10240));
    return sonare::test::render_left(synth, 8192);
  };
  const bool pitch_frozen = render(false, true) == render(false, false);
  REQUIRE(pitch_frozen);
  const std::vector<float> baseline = render(true, false);
  REQUIRE(sonare::test::rms(baseline) > 1.0e-6f);
  const bool matrix_frozen = render(true, true) == baseline;
  REQUIRE(matrix_frozen);
}

TEST_CASE("MPE prohibited messages cannot bypass ignore rules through a controller profile",
          "[midi][mpe]") {
  struct Input {
    bool pressure;
    uint8_t controller;
    uint8_t channel;
  };
  for (const bool midi2 : {false, true}) {
    for (const Input input : {Input{true, 0, 2}, Input{false, 0, 2}, Input{false, 32, 2},
                              Input{false, 126, 0}, Input{false, 127, 0}}) {
      DYNAMIC_SECTION("midi2=" << midi2 << " pressure=" << input.pressure
                               << " controller=" << +input.controller) {
        CAPTURE(input.pressure, input.controller, input.channel);
        const bool refused = render_profile_input(true, true, input.pressure, input.controller,
                                                  input.channel, midi2) ==
                             render_profile_input(true, false, input.pressure, input.controller,
                                                  input.channel, midi2);
        REQUIRE(refused);
        const bool accepted = render_profile_input(false, true, input.pressure, input.controller,
                                                   input.channel, midi2) !=
                              render_profile_input(false, false, input.pressure, input.controller,
                                                   input.channel, midi2);
        REQUIRE(accepted);
      }
    }
  }
}
