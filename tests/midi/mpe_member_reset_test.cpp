#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "midi/controller_profile.h"
#include "midi/synth/native_synth.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
namespace {
enum class ResetInput { kMemberZero, kMemberReset, kBothZero };

std::vector<float> held_manager_reset(ResetInput input, bool shared_axis = false,
                                      bool ordinary_alias = false) {
  using namespace sonare::midi;
  using namespace sonare::midi::synth;
  NativeSynthConfig cfg;
  cfg.patch.mode = SynthEngineMode::kReed;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  if (shared_axis) {
    REQUIRE(profile.bind(
        {ControllerInput::kControlChange, kMpeTimbreCc, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  }
  if (ordinary_alias) {
    REQUIRE(profile.bind(
        {ControllerInput::kControlChange, 2, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  }
  REQUIRE(synth.set_controller_profile(profile));
  synth.prepare(48000.0, 256);
  const auto send = [&](const Ump& u) { synth.on_event(0, sonare::test::event(u)); };
  send(make_midi1_control_change(0, 0, 101, 0));
  send(make_midi1_control_change(0, 0, 100, 6));
  send(make_midi1_control_change(0, 0, 6, 7));
  send(make_midi1_note_on(0, 2, 60, 100));
  sonare::test::render_left(synth, 4096);
  if (shared_axis) send(make_midi1_control_change(0, 0, kMpeTimbreCc, 80));
  send(make_midi1_channel_pressure(0, 0, 70));
  if (shared_axis) {
    send(make_midi1_control_change(0, 2, kMpeTimbreCc, 20));
  } else {
    send(make_midi1_channel_pressure(0, 2, 20));
  }
  if (ordinary_alias) send(make_midi1_control_change(0, 2, 2, 30));
  sonare::test::render_left(synth, 4096);
  if (input == ResetInput::kMemberReset) {
    send(make_midi1_control_change(0, 2, 121, 0));
  } else {
    if (shared_axis) {
      send(make_midi1_control_change(0, 2, kMpeTimbreCc, 0));
    } else {
      send(make_midi1_channel_pressure(0, 2, 0));
    }
    if (input == ResetInput::kBothZero) {
      send(make_midi1_channel_pressure(0, 0, 0));
      if (shared_axis) send(make_midi1_control_change(0, 0, kMpeTimbreCc, 0));
    }
  }
  return sonare::test::render_left(synth, 8192);
}
float max_delta(const std::vector<float>& a, const std::vector<float>& b) {
  float delta = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) delta = std::max(delta, std::fabs(a[i] - b[i]));
  return delta;
}
}  // namespace
TEST_CASE("member reset preserves manager engine excitation on a held voice", "[midi][mpe]") {
  const auto manager_only = held_manager_reset(ResetInput::kMemberZero);
  const auto reset = held_manager_reset(ResetInput::kMemberReset);
  const auto no_pressure = held_manager_reset(ResetInput::kBothZero);
  const float manager_survival_delta = max_delta(reset, manager_only);
  const float manager_contribution = max_delta(manager_only, no_pressure);
  CAPTURE(manager_survival_delta, manager_contribution);
  REQUIRE(sonare::test::rms(manager_only) > 1.0e-6f);
  REQUIRE(sonare::test::rms(reset) > 1.0e-6f);
  REQUIRE(sonare::test::rms(no_pressure) > 1.0e-6f);
  REQUIRE(manager_contribution > 1.0e-5f);
  REQUIRE(reset == manager_only);
}

TEST_CASE("member reset preserves the last MPE writer on shared engine axes", "[midi][mpe]") {
  const auto manager_only = held_manager_reset(ResetInput::kMemberZero, true);
  const auto reset = held_manager_reset(ResetInput::kMemberReset, true);
  const auto no_pressure = held_manager_reset(ResetInput::kBothZero, true);
  CAPTURE(max_delta(reset, manager_only), max_delta(manager_only, no_pressure));
  REQUIRE(sonare::test::rms(manager_only) > 1.0e-6f);
  REQUIRE(sonare::test::rms(reset) > 1.0e-6f);
  REQUIRE(max_delta(manager_only, no_pressure) > 1.0e-5f);
  REQUIRE(reset == manager_only);
}

TEST_CASE("member reset restores a manager axis obscured by an ordinary controller",
          "[midi][mpe]") {
  const auto manager_only = held_manager_reset(ResetInput::kMemberZero, false, true);
  const auto reset = held_manager_reset(ResetInput::kMemberReset, false, true);
  const auto no_pressure = held_manager_reset(ResetInput::kBothZero, false, true);
  CAPTURE(max_delta(reset, manager_only), max_delta(manager_only, no_pressure));
  REQUIRE(sonare::test::rms(manager_only) > 1.0e-6f);
  REQUIRE(sonare::test::rms(reset) > 1.0e-6f);
  REQUIRE(max_delta(manager_only, no_pressure) > 1.0e-5f);
  REQUIRE(reset == manager_only);
}
