/// @file controller_profile_test.cpp
/// @brief That which controller carries a gesture stops mattering: the same
///        breath ramp reaches the same axes whether a device spells it CC2 or
///        channel aftertouch, and a synth given no profile sounds as before.
///
/// Two levels, because they can fail apart. The resolver level compares the
/// (axis, value) sequences two inputs produce, which is the claim the layer
/// itself makes. The render level compares the audio, which is the claim a host
/// cares about and the only one that can catch the mapping arriving correctly
/// and then being applied somewhere else.
///
/// The reach counters are not decoration. A resolver that stopped matching any
/// binding would emit nothing, and two empty sequences compare equal; a ramp
/// whose messages never reached the synth would render two identical buffers.
/// Both comparisons therefore carry the number of values they actually saw.

#include "midi/controller_profile.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::controller_input_of;
using sonare::midi::ControllerAxis;
using sonare::midi::ControllerAxisValue;
using sonare::midi::ControllerInput;
using sonare::midi::ControllerInputValue;
using sonare::midi::ControllerProfile;
using sonare::midi::kMaxControllerBindings;
using sonare::midi::Ump;
using sonare::midi::synth::default_controller_profile;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::test::event;
using sonare::test::render_left;
using sonare::test::rms;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kNote = 60;
constexpr int kVelocity = 100;

/// Frames rendered before the ramp starts, and per ramp step. Same shape as the
/// excitation-axis measurement: long enough that the onset is over and that each
/// step outlasts the control glide many times over.
constexpr int kPrefillFrames = 16384;
constexpr int kSteps = 8;
constexpr int kStepFrames = 4096;

ControllerProfile named(const char* name) {
  ControllerProfile profile;
  REQUIRE(ControllerProfile::preset(name, &profile));
  return profile;
}

/// Every axis value a ramp produced, flattened in emission order.
std::vector<ControllerAxisValue> resolve_ramp(const ControllerProfile& profile,
                                              const std::vector<Ump>& messages) {
  std::vector<ControllerAxisValue> out;
  std::array<ControllerAxisValue, kMaxControllerBindings> scratch{};
  for (const Ump& message : messages) {
    const size_t count = profile.resolve(message, scratch.data(), scratch.size());
    for (size_t i = 0; i < count; ++i) out.push_back(scratch[i]);
  }
  return out;
}

std::vector<Ump> cc_ramp(uint8_t controller) {
  std::vector<Ump> out;
  for (int value = 0; value <= 127; ++value) {
    out.push_back(
        sonare::midi::make_midi1_control_change(0, 0, controller, static_cast<uint8_t>(value)));
  }
  return out;
}

std::vector<Ump> aftertouch_ramp() {
  std::vector<Ump> out;
  for (int value = 0; value <= 127; ++value) {
    out.push_back(sonare::midi::make_midi1_channel_pressure(0, 0, static_cast<uint8_t>(value)));
  }
  return out;
}

NativeSynth make_synth(int program, const ControllerProfile& profile) {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  NativeSynth synth(cfg);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  synth.on_event(
      0, event(sonare::midi::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  return synth;
}

/// Renders @p program while stepping one input from silence to full. @p step
/// builds the message for a 0..127 position, so the two spellings of one gesture
/// differ in nothing but that.
std::vector<float> render_ramp(int program, const ControllerProfile& profile,
                               Ump (*step)(uint8_t)) {
  NativeSynth synth = make_synth(program, profile);
  std::vector<float> out = render_left(synth, kPrefillFrames);
  for (int i = 0; i < kSteps; ++i) {
    const int value = ((i + 1) * 127) / kSteps;
    synth.on_event(0, event(step(static_cast<uint8_t>(value))));
    const std::vector<float> block = render_left(synth, kStepFrames);
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

Ump breath_cc_step(uint8_t value) {
  return sonare::midi::make_midi1_control_change(0, 0, 2, value);
}
Ump aftertouch_step(uint8_t value) {
  return sonare::midi::make_midi1_channel_pressure(0, 0, value);
}

/// Programs covering the engines a breath gesture can reach, so the comparison
/// is not one engine's accident.
constexpr int kWindPrograms[] = {21, 40, 56, 65, 73};

}  // namespace

TEST_CASE("one gesture resolves the same whichever controller carries it",
          "[midi][synth][controller-profile]") {
  const std::vector<ControllerAxisValue> by_cc = resolve_ramp(named("breath"), cc_ramp(2));
  const std::vector<ControllerAxisValue> by_aftertouch =
      resolve_ramp(named("breath-aftertouch"), aftertouch_ramp());

  // Reach: two profiles that matched nothing would both emit nothing and
  // compare equal. 128 ramp positions, two axes bound to the gesture.
  CAPTURE(by_cc.size(), by_aftertouch.size());
  REQUIRE(by_cc.size() == 256);
  REQUIRE(by_aftertouch.size() == by_cc.size());

  for (size_t i = 0; i < by_cc.size(); ++i) {
    CAPTURE(i);
    REQUIRE(by_cc[i].axis == by_aftertouch[i].axis);
    REQUIRE(by_cc[i].value == by_aftertouch[i].value);
    REQUIRE(by_cc[i].note == by_aftertouch[i].note);
  }
}

TEST_CASE("a breath ramp sounds the same through either spelling",
          "[midi][synth][controller-profile]") {
  const ControllerProfile by_cc = named("breath");
  const ControllerProfile by_aftertouch = named("breath-aftertouch");
  for (const int program : kWindPrograms) {
    CAPTURE(program);
    const std::vector<float> cc_render = render_ramp(program, by_cc, breath_cc_step);
    const std::vector<float> at_render = render_ramp(program, by_aftertouch, aftertouch_step);
    REQUIRE(cc_render.size() == at_render.size());

    // Reach: two silent renders are identical too, and so are two renders the
    // gesture never touched. The control holds the same profile and receives no
    // message, so the only difference between it and the ramp is the ramp.
    NativeSynth untouched = make_synth(program, by_cc);
    const std::vector<float> control =
        render_left(untouched, kPrefillFrames + kSteps * kStepFrames);
    REQUIRE(rms(cc_render) > 0.0f);
    REQUIRE(control.size() == cc_render.size());
    bool moved = false;
    for (size_t i = 0; i < cc_render.size() && !moved; ++i) {
      moved = cc_render[i] != control[i];
    }
    REQUIRE(moved);

    for (size_t i = 0; i < cc_render.size(); ++i) {
      if (cc_render[i] != at_render[i]) {
        CAPTURE(i, cc_render[i], at_render[i]);
        FAIL("the two spellings of one gesture rendered different samples");
      }
    }
  }
}

TEST_CASE("a profile with no binding leaves the controller unheard",
          "[midi][synth][controller-profile]") {
  // The other side of the previous case: with nothing bound, the same ramp has
  // to change not one sample. Exact equality rather than a threshold, because
  // the CC never reaches a setter at all -- the two renders are the same
  // arithmetic over the same state.
  const ControllerProfile unbound;
  REQUIRE(unbound.binding_count() == 0);
  for (const int program : kWindPrograms) {
    CAPTURE(program);
    const std::vector<float> ramped = render_ramp(program, unbound, breath_cc_step);
    NativeSynth quiet = make_synth(program, unbound);
    const std::vector<float> control = render_left(quiet, kPrefillFrames + kSteps * kStepFrames);
    REQUIRE(ramped.size() == control.size());
    for (size_t i = 0; i < ramped.size(); ++i) {
      if (ramped[i] != control[i]) {
        CAPTURE(i);
        FAIL("an unbound controller moved the sound");
      }
    }
  }
}

TEST_CASE("the default profile is the gm preset", "[midi][synth][controller-profile]") {
  const ControllerProfile fallback = default_controller_profile();
  const ControllerProfile gm = named("gm");
  REQUIRE(fallback.binding_count() == gm.binding_count());
  REQUIRE(fallback.binding_count() == 3);
  REQUIRE(fallback.velocity_meaningful == gm.velocity_meaningful);
  for (size_t i = 0; i < gm.binding_count(); ++i) {
    CAPTURE(i);
    REQUIRE(fallback.binding_at(i).input == gm.binding_at(i).input);
    REQUIRE(fallback.binding_at(i).index == gm.binding_at(i).index);
    REQUIRE(fallback.binding_at(i).axis == gm.binding_at(i).axis);
  }
  // A synth that was never told a profile answers the same one.
  NativeSynthConfig cfg;
  NativeSynth synth(cfg);
  REQUIRE(synth.controller_profile()->binding_count() == gm.binding_count());
}

TEST_CASE("every named preset resolves and an unnamed one refuses",
          "[midi][synth][controller-profile]") {
  size_t resolved = 0;
  for (size_t i = 0; i < ControllerProfile::preset_count(); ++i) {
    const char* name = ControllerProfile::preset_name_at(i);
    REQUIRE(name != nullptr);
    CAPTURE(std::string(name));
    ControllerProfile profile;
    REQUIRE(ControllerProfile::preset(name, &profile));
    REQUIRE(profile.binding_count() > 0);
    ++resolved;
  }
  REQUIRE(resolved == ControllerProfile::preset_count());
  REQUIRE(resolved == 4);
  REQUIRE(ControllerProfile::preset_name_at(resolved) == nullptr);

  // An unknown name leaves the caller's profile alone rather than handing back
  // a default: a device spelling silently replaced by another is exactly what
  // this layer exists to make impossible.
  ControllerProfile kept = named("breath");
  const size_t before = kept.binding_count();
  REQUIRE_FALSE(ControllerProfile::preset("ewi", &kept));
  REQUIRE(kept.binding_count() == before);
}

TEST_CASE("bind refuses what it cannot honour", "[midi][synth][controller-profile]") {
  ControllerProfile profile;
  REQUIRE_FALSE(profile.bind({ControllerInput::kControlChange, 2, ControllerAxis::kNone}));

  // A per-note value can reach an engine's own exciter and nothing else here;
  // the other three axes are channel state, and widening the binding silently
  // is what would make a refusal indistinguishable from a success.
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kExcitation}));
  REQUIRE_FALSE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kLoudness}));
  REQUIRE_FALSE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kPitchCents}));

  ControllerProfile full;
  size_t accepted = 0;
  for (size_t i = 0; i < kMaxControllerBindings + 4; ++i) {
    const bool ok = full.bind({ControllerInput::kControlChange, static_cast<uint8_t>(i % 128),
                               ControllerAxis::kBrightness});
    if (ok) ++accepted;
  }
  REQUIRE(accepted == kMaxControllerBindings);
  REQUIRE(full.binding_count() == kMaxControllerBindings);
}

TEST_CASE("a binding's range and curve shape the axis", "[midi][synth][controller-profile]") {
  ControllerProfile profile;
  // Inverted range: full deflection lands at the bottom of the axis.
  REQUIRE(
      profile.bind({ControllerInput::kControlChange, 2, ControllerAxis::kBrightness, 1.0f, 0.0f}));
  // Cents rather than a normalized axis, and a square-law curve on top.
  REQUIRE(profile.bind(
      {ControllerInput::kControlChange, 2, ControllerAxis::kPitchCents, 0.0f, 1200.0f, 2.0f}));

  std::array<ControllerAxisValue, kMaxControllerBindings> out{};
  const size_t count = profile.resolve(sonare::midi::make_midi1_control_change(0, 0, 2, 127),
                                       out.data(), out.size());
  REQUIRE(count == 2);
  REQUIRE(out[0].axis == ControllerAxis::kBrightness);
  REQUIRE(out[0].value == 0.0f);
  REQUIRE(out[1].axis == ControllerAxis::kPitchCents);
  REQUIRE(out[1].value == 1200.0f);

  const size_t half =
      profile.resolve(sonare::midi::make_midi1_control_change(0, 0, 2, 64), out.data(), out.size());
  REQUIRE(half == 2);
  const float norm = 64.0f / 127.0f;
  REQUIRE(out[0].value == 1.0f - norm);
  REQUIRE(out[1].value == 1200.0f * norm * norm);
}

TEST_CASE("an input decodes to the gesture it carries", "[midi][synth][controller-profile]") {
  ControllerInputValue in{};
  REQUIRE(controller_input_of(sonare::midi::make_midi1_control_change(0, 3, 74, 127), &in));
  REQUIRE(in.input == ControllerInput::kControlChange);
  REQUIRE(in.index == 74);
  REQUIRE(in.channel == 3);
  REQUIRE(in.norm == 1.0f);

  REQUIRE(controller_input_of(sonare::midi::make_midi1_channel_pressure(0, 0, 127), &in));
  REQUIRE(in.input == ControllerInput::kChannelPressure);
  REQUIRE(in.norm == 1.0f);

  REQUIRE(controller_input_of(sonare::midi::make_midi1_poly_pressure(0, 0, kNote, 64), &in));
  REQUIRE(in.input == ControllerInput::kPolyPressure);
  REQUIRE(in.note == kNote);

  REQUIRE(controller_input_of(sonare::midi::make_midi1_note_on(0, 0, kNote, 100), &in));
  REQUIRE(in.input == ControllerInput::kVelocity);

  // Centred rather than signed, so one range spelling serves every input.
  REQUIRE(controller_input_of(sonare::midi::make_midi1_pitch_bend(0, 0, 8192), &in));
  REQUIRE(in.input == ControllerInput::kPitchBend);
  REQUIRE(std::abs(in.norm - 0.5f) < 1.0e-4f);

  REQUIRE_FALSE(controller_input_of(sonare::midi::make_midi1_program_change(0, 0, 40), &in));
}

TEST_CASE("the three channel axes reach the sound", "[midi][synth][controller-profile]") {
  // Each is declared in the axis enumeration, so each needs a consumer: an axis
  // a caller can name and nothing reads is the failure mode this layer was
  // written against.
  struct ChannelAxisProbe {
    ControllerAxis axis;
    float lo;
    float hi;
    const char* label;
  };
  constexpr ChannelAxisProbe kProbes[] = {
      {ControllerAxis::kLoudness, 1.0f, 0.0f, "loudness"},
      {ControllerAxis::kPitchCents, 0.0f, 1200.0f, "pitch cents"},
      {ControllerAxis::kVibratoDepth, 0.0f, 600.0f, "vibrato depth"},
  };

  size_t probed = 0;
  for (const ChannelAxisProbe& probe : kProbes) {
    CAPTURE(probe.label);
    ControllerProfile profile;
    REQUIRE(profile.bind({ControllerInput::kControlChange, 2, probe.axis, probe.lo, probe.hi}));

    NativeSynth moved = make_synth(0, profile);
    NativeSynth still = make_synth(0, profile);
    render_left(moved, kPrefillFrames);
    render_left(still, kPrefillFrames);
    moved.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 2, 127)));

    const std::vector<float> a = render_left(moved, kStepFrames);
    const std::vector<float> b = render_left(still, kStepFrames);
    std::vector<float> diff(a.size(), 0.0f);
    for (size_t i = 0; i < a.size(); ++i) diff[i] = a[i] - b[i];
    CAPTURE(rms(a), rms(b), rms(diff));
    REQUIRE(rms(b) > 0.0f);
    REQUIRE(rms(diff) > 0.0f);
    ++probed;
  }
  REQUIRE(probed == std::size(kProbes));
}
