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

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/wind_breath.h"
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
using sonare::midi::synth::ExcitationAxes;
using sonare::midi::synth::ExcitationBases;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::kAxisBrightness;
using sonare::midi::synth::kAxisForce;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
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

NativeSynth make_synth(int program, const ControllerProfile& profile, bool start_note = true) {
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
  if (start_note) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  }
  return synth;
}

NativeSynth make_patch_synth(const NativeSynthPatch& patch, const ControllerProfile& profile,
                             bool start_note = true) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  cfg.use_gm_programs = false;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  NativeSynth synth(cfg);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  if (start_note) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  }
  return synth;
}

NativeSynthPatch reset_patch(int program) {
  NativeSynthPatch patch = gm_fallback_patch(0, static_cast<uint8_t>(program));
  if (program == 16) {
    // Give the organ's zero drawbars_b a real morph end so the control is not inert.
    patch.additive.drawbars_b = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 8.0f};
  }
  return patch;
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

Ump controller_step(uint8_t controller, uint8_t value) {
  return sonare::midi::make_midi1_control_change(0, 0, controller, value);
}

Ump poly_pressure_step(uint8_t value) {
  return sonare::midi::make_midi1_poly_pressure(0, 0, kNote, value);
}

void start_note(NativeSynth& synth) {
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
}

ControllerProfile single_cc16_profile(ControllerAxis axis) {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kControlChange, 16, axis}));
  return profile;
}

/// Programs covering the engines a breath gesture can reach, so the comparison
/// is not one engine's accident.
constexpr int kWindPrograms[] = {21, 40, 56, 65, 73};

struct EngineAxisCase {
  int program;
  ControllerAxis axis;
  const char* label;
};

/// Every continuously controlled GM fallback, plus the additive registration
/// morph. These are the engines whose active voices must follow a reset to the
/// same base they used when the note began.
constexpr EngineAxisCase kResetAxisCases[] = {
    {16, ControllerAxis::kMorph, "additive morph"},
    {19, ControllerAxis::kExcitation, "pipe organ excitation"},
    {40, ControllerAxis::kPosition, "bowed string position"},
    {52, ControllerAxis::kBrightness, "vocal brightness"},
    {56, ControllerAxis::kBrightness, "brass brightness"},
    {65, ControllerAxis::kExcitation, "reed excitation"},
    {73, ControllerAxis::kExcitation, "flute excitation"},
    {21, ControllerAxis::kBrightness, "free reed brightness"},
};

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

TEST_CASE("MIDI 1.0 input normalizes exactly as the 7- and 14-bit fields always did",
          "[midi][controller-profile]") {
  ControllerInputValue in{};
  for (int v = 0; v < 128; ++v) {
    const float expected = static_cast<float>(v) / 127.0f;
    const auto value = static_cast<uint8_t>(v);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_control_change(0, 0, 11, value), &in));
    REQUIRE(in.norm == expected);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_channel_pressure(0, 0, value), &in));
    REQUIRE(in.norm == expected);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_poly_pressure(0, 0, kNote, value), &in));
    REQUIRE(in.norm == expected);
    if (v > 0) {
      REQUIRE(controller_input_of(sonare::midi::make_midi1_note_on(0, 0, kNote, value), &in));
      REQUIRE(in.input == ControllerInput::kVelocity);
      REQUIRE(in.norm == expected);
    }
  }
  for (int v = 0; v < 16384; ++v) {
    REQUIRE(controller_input_of(sonare::midi::make_midi1_pitch_bend(0, 0, static_cast<uint16_t>(v)),
                                &in));
    REQUIRE(in.norm == static_cast<float>(v) / 16383.0f);
  }
}

TEST_CASE("MIDI 2.0 input lands on the same scale as the equal MIDI 1.0 value",
          "[midi][controller-profile]") {
  ControllerInputValue in1{};
  ControllerInputValue in2{};
  for (int v = 1; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    const uint32_t up32 = sonare::midi::scale_cc_7_to_32(v7);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_control_change(0, 0, 11, v7), &in1));
    REQUIRE(controller_input_of(sonare::midi::make_midi2_control_change(0, 0, 11, up32), &in2));
    REQUIRE(in2.norm == in1.norm);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_channel_pressure(0, 0, v7), &in1));
    REQUIRE(controller_input_of(sonare::midi::make_midi2_channel_pressure(0, 0, up32), &in2));
    REQUIRE(in2.norm == in1.norm);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_note_on(0, 0, kNote, v7), &in1));
    REQUIRE(controller_input_of(
        sonare::midi::make_midi2_note_on(0, 0, kNote, sonare::midi::scale_velocity_7_to_16(v7)),
        &in2));
    REQUIRE(in2.input == ControllerInput::kVelocity);
    REQUIRE(in2.norm == in1.norm);
  }
  for (int v = 0; v < 16384; v += 37) {
    const auto v14 = static_cast<uint16_t>(v);
    REQUIRE(controller_input_of(sonare::midi::make_midi1_pitch_bend(0, 0, v14), &in1));
    REQUIRE(controller_input_of(
        sonare::midi::make_midi2_pitch_bend(0, 0, sonare::midi::scale_bend_14_to_32(v14)), &in2));
    REQUIRE(in2.norm == in1.norm);
  }
}

TEST_CASE("a MIDI 2.0 midpoint lies strictly between the neighbouring MIDI 1.0 values",
          "[midi][controller-profile]") {
  ControllerInputValue lo{};
  ControllerInputValue mid{};
  ControllerInputValue hi{};
  const uint32_t a = sonare::midi::scale_cc_7_to_32(40);
  const uint32_t b = sonare::midi::scale_cc_7_to_32(41);
  REQUIRE(controller_input_of(sonare::midi::make_midi2_control_change(0, 0, 11, a), &lo));
  REQUIRE(controller_input_of(sonare::midi::make_midi2_control_change(0, 0, 11, a + (b - a) / 2),
                              &mid));
  REQUIRE(controller_input_of(sonare::midi::make_midi2_control_change(0, 0, 11, b), &hi));
  REQUIRE(lo.norm < mid.norm);
  REQUIRE(mid.norm < hi.norm);
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

TEST_CASE("reset all controllers restores every active engine axis to its note baseline",
          "[midi][synth][controller-profile]") {
  for (const EngineAxisCase& axis_case : kResetAxisCases) {
    CAPTURE(axis_case.label, axis_case.program);
    const ControllerProfile profile = single_cc16_profile(axis_case.axis);
    const NativeSynthPatch patch = reset_patch(axis_case.program);
    NativeSynth target = make_patch_synth(patch, profile);
    NativeSynth latched = make_patch_synth(patch, profile);
    NativeSynth untouched = make_patch_synth(patch, profile);

    render_left(target, kPrefillFrames);
    render_left(latched, kPrefillFrames);
    render_left(untouched, kPrefillFrames);

    const Ump full = controller_step(16, 127);
    target.on_event(0, event(full));
    latched.on_event(0, event(full));
    const std::vector<float> target_moved = render_left(target, kStepFrames);
    const std::vector<float> latched_moved = render_left(latched, kStepFrames);
    const std::vector<float> untouched_output = render_left(untouched, kStepFrames);

    // Scalar booleans keep a failure from dumping three buffers; untouched proves CC16 arrived.
    const bool target_matches_latched = target_moved == latched_moved;
    const bool moved_from_untouched = target_moved != untouched_output;
    CHECK(target_matches_latched);
    CHECK(moved_from_untouched);

    target.on_event(0, event(controller_step(121, 0)));
    const std::vector<float> target_after_reset = render_left(target, kPrefillFrames);
    const std::vector<float> latched_after_reset = render_left(latched, kPrefillFrames);
    // Reset must move the active engine off the latched axis value.
    const bool reset_released_active_latch = target_after_reset != latched_after_reset;
    CHECK(reset_released_active_latch);
  }
}

TEST_CASE("replacing a controller profile restores the active reed baseline",
          "[midi][synth][controller-profile]") {
  const ControllerProfile profile = single_cc16_profile(ControllerAxis::kExcitation);
  const ControllerProfile empty;
  NativeSynth target = make_synth(65, profile);
  NativeSynth latched = make_synth(65, profile);
  NativeSynth untouched = make_synth(65, profile);

  render_left(target, kPrefillFrames);
  render_left(latched, kPrefillFrames);
  render_left(untouched, kPrefillFrames);
  const Ump full = controller_step(16, 127);
  target.on_event(0, event(full));
  latched.on_event(0, event(full));
  const std::vector<float> target_moved = render_left(target, kStepFrames);
  const std::vector<float> latched_moved = render_left(latched, kStepFrames);
  const std::vector<float> untouched_output = render_left(untouched, kStepFrames);
  const bool target_matches_latched = target_moved == latched_moved;
  const bool moved_from_untouched = target_moved != untouched_output;
  CHECK(target_matches_latched);
  CHECK(moved_from_untouched);

  target.set_controller_profile(empty);
  const std::vector<float> target_after_replacement = render_left(target, kPrefillFrames);
  const std::vector<float> latched_after_replacement = render_left(latched, kPrefillFrames);
  // Replacing the profile must release the active axis latch.
  const bool replacement_released_active_latch =
      target_after_replacement != latched_after_replacement;
  CHECK(replacement_released_active_latch);
}

TEST_CASE("reset all controllers returns a reed to its precise note-on baseline",
          "[midi][synth][controller-profile]") {
  NativeSynthPatch patch = reset_patch(65);
  patch.reed.breath_pressure = 0.3f;
  patch.reed.vel_to_breath = 0.0f;
  ControllerProfile profile;
  REQUIRE(
      profile.bind({ControllerInput::kControlChange, 16, ControllerAxis::kExcitation, 0.3f, 1.0f}));

  NativeSynth target = make_patch_synth(patch, profile);
  NativeSynth reference = make_patch_synth(patch, profile);
  render_left(target, kPrefillFrames);
  render_left(reference, kPrefillFrames);

  const Ump full = controller_step(16, 127);
  target.on_event(0, event(full));
  reference.on_event(0, event(full));
  render_left(target, kPrefillFrames);
  render_left(reference, kPrefillFrames);

  // CC16=0 maps to the patch's .3 baseline, the same target the reset must restore.
  target.on_event(0, event(controller_step(121, 0)));
  reference.on_event(0, event(controller_step(16, 0)));
  const std::vector<float> target_after_reset = render_left(target, kPrefillFrames);
  const std::vector<float> reference_at_baseline = render_left(reference, kPrefillFrames);
  const bool reset_matches_note_baseline = target_after_reset == reference_at_baseline;
  CHECK(reset_matches_note_baseline);
}

TEST_CASE("an unrelated channel axis does not overwrite a per-note excitation",
          "[midi][synth][controller-profile]") {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kControlChange, 16, ControllerAxis::kExcitation}));
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kExcitation}));
  REQUIRE(profile.bind({ControllerInput::kControlChange, 17, ControllerAxis::kBrightness}));

  NativeSynth target = make_synth(40, profile, false);
  NativeSynth peer = make_synth(40, profile, false);
  NativeSynth control = make_synth(40, profile, false);
  const Ump low_force = controller_step(16, 0);
  target.on_event(0, event(low_force));
  peer.on_event(0, event(low_force));
  control.on_event(0, event(low_force));
  start_note(target);
  start_note(peer);
  start_note(control);
  render_left(target, kPrefillFrames);
  render_left(peer, kPrefillFrames);
  render_left(control, kPrefillFrames);

  const Ump high_per_note_force = poly_pressure_step(127);
  target.on_event(0, event(high_per_note_force));
  peer.on_event(0, event(high_per_note_force));
  const std::vector<float> target_force = render_left(target, kStepFrames);
  const std::vector<float> peer_force = render_left(peer, kStepFrames);
  const std::vector<float> control_force = render_left(control, kStepFrames);
  const bool per_note_target_matches_peer = target_force == peer_force;
  const bool per_note_force_changed = target_force != control_force;
  CHECK(per_note_target_matches_peer);
  CHECK(per_note_force_changed);

  // Bowed string declines brightness; applying it must not re-push channel force.
  target.on_event(0, event(controller_step(17, 127)));
  const std::vector<float> target_after_brightness = render_left(target, kPrefillFrames);
  const std::vector<float> peer_after_brightness = render_left(peer, kPrefillFrames);
  const bool unrelated_axis_preserved = target_after_brightness == peer_after_brightness;
  CHECK(unrelated_axis_preserved);
}

TEST_CASE("multiple per-note bindings retain each resolved excitation axis",
          "[midi][synth][controller-profile]") {
  NativeSynthPatch patch = reset_patch(65);
  patch.mod_matrix = {};
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kControlChange, 16, ControllerAxis::kExcitation}));
  REQUIRE(profile.bind({ControllerInput::kControlChange, 17, ControllerAxis::kBrightness}));
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kExcitation}));
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kBrightness}));

  NativeSynth target = make_patch_synth(patch, profile, false);
  NativeSynth reference = make_patch_synth(patch, profile, false);
  NativeSynth untouched = make_patch_synth(patch, profile, false);
  const Ump low_force = controller_step(16, 0);
  const Ump low_brightness = controller_step(17, 0);
  for (NativeSynth* synth : {&target, &reference, &untouched}) {
    synth->on_event(0, event(low_force));
    synth->on_event(0, event(low_brightness));
    start_note(*synth);
    render_left(*synth, kPrefillFrames);
  }

  // The two PolyPressure bindings are one gesture that resolves to both axes.
  // The reference spells the same final state as two channel-wide writes.
  target.on_event(0, event(poly_pressure_step(127)));
  reference.on_event(0, event(controller_step(16, 127)));
  reference.on_event(0, event(controller_step(17, 127)));
  const std::vector<float> target_output = render_left(target, kStepFrames);
  const std::vector<float> reference_output = render_left(reference, kStepFrames);
  const std::vector<float> untouched_output = render_left(untouched, kStepFrames);
  const bool per_note_axes_match = target_output == reference_output;
  const bool per_note_axes_reached_audio = target_output != untouched_output;
  CHECK(per_note_axes_match);
  CHECK(per_note_axes_reached_audio);
}

TEST_CASE("restoring excitation bases retains matrix offsets",
          "[midi][synth][controller-profile]") {
  ExcitationBases bases{0.2f, 0.4f, 0.1f, -0.1f};
  ExcitationAxes live{0.9f, 0.0f, 0.8f, 0.0f};
  bases.set_base(live, kAxisForce | kAxisBrightness);
  ExcitationAxes offsets{0.3f, 0.0f, -0.2f, 0.0f};
  bases.set_mod(offsets);
  bases.restore_base();

  const bool bases_restored = bases.force01_base == 0.2f && bases.bright01_base == 0.4f;
  const bool offsets_retained = bases.force_mod01 == 0.3f && bases.bright_mod01 == -0.2f;
  CHECK(bases_restored);
  CHECK(offsets_retained);
}
