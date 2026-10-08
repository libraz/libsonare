/// @file mpe_native_synth_test.cpp
/// @brief The synth under an MPE zone: what the MCM changes about a bend, what
///        the manager channel reaches, and what the zone makes the synth ignore.
///
/// Three of these are audible and measured as pitch, because a bend range is
/// the one obligation whose breach a listener hears. The rest are refusals, and
/// a refusal sounds exactly like a message that never arrived -- so each of
/// those cases pairs the refused channel with a channel where the same message
/// is accepted, and requires the two to differ. Without the pair the case would
/// pass on a synth that ignored the message everywhere.
///
/// One prohibition is deliberately absent. Bank select on a member channel is
/// unobservable here by construction: the bank is only ever read when a program
/// change resolves, and in the one configuration where the bank is refused --
/// a member channel in MIDI Mode 3 -- the program change is refused too, while
/// Mode 4 accepts both. The guard is a conformance one with no audible arm, so
/// it is tested where it can be: MpeState::ignores, in mpe_zones_test.cpp.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "midi/articulation_mode.h"
#include "midi/controller_profile.h"
#include "midi/mpe.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::ControllerAxis;
using sonare::midi::ControllerInput;
using sonare::midi::ControllerProfile;
using sonare::midi::kMpeTimbreCc;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::Ump;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::fft_fundamental;
using sonare::test::render_left;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr uint8_t kNote = 60;
constexpr double kNoteHz = 261.6255653;
constexpr uint8_t kVelocity = 100;

/// A quarter of the way up from centre, which is +0.5 semitones at the ordinary
/// +/-2 range and +12 at the zone's +/-48.
constexpr uint16_t kBendUp = 8192 + 2048;

NativeSynth make_synth(bool dc_block = true) {
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.mode = SynthEngineMode::kReed;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.dc_block = dc_block;
  return NativeSynth(cfg);
}

void send(NativeSynth& synth, const sonare::midi::Ump& ump) { synth.on_event(0, event(ump)); }

/// The MPE Configuration Message: RPN 00 06 with the member count in Data Entry
/// MSB.
void send_mcm(NativeSynth& synth, uint8_t manager_channel, uint8_t members) {
  send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 101, 0));
  send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 100, 6));
  send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 6, members));
}

/// Sounding pitch of a note on @p channel bent by @p bend14, in cents from the
/// unbent note. @p expect_cents seeds the analysis, which needs a neighbourhood
/// rather than a fundamental: a reed's third harmonic is louder than its first
/// and a seed at the wrong octave locks onto it.
double bent_cents(NativeSynth& synth, uint8_t channel, uint16_t bend14, double expect_cents) {
  send(synth, sonare::midi::make_midi1_pitch_bend(0, channel, bend14));
  send(synth, sonare::midi::make_midi1_note_on(0, channel, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  send(synth, sonare::midi::make_midi1_note_off(0, channel, kNote, 0));
  const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, expect_cents / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

/// One of the two dimensions whose values combine inside the controller domain,
/// with the profile binding that carries it to an axis.
struct FoldedDimension {
  const char* label;
  ControllerInput input;
  uint8_t index;
  Ump (*step)(uint8_t channel, uint8_t value);
};

struct VelocityLaneRender {
  std::vector<float> first;
  std::vector<float> second;
};

Ump timbre_step(uint8_t channel, uint8_t value) {
  return sonare::midi::make_midi1_control_change(0, channel, kMpeTimbreCc, value);
}
Ump pressure_step(uint8_t channel, uint8_t value) {
  return sonare::midi::make_midi1_channel_pressure(0, channel, value);
}

constexpr FoldedDimension kFolded[] = {
    {"timbre", ControllerInput::kControlChange, kMpeTimbreCc, timbre_step},
    {"pressure", ControllerInput::kChannelPressure, 0, pressure_step},
};

/// Cents the note on channel 2 sounds at with the dimension bound to pitch and
/// the manager and the member each sending @p manager_value / @p member_value.
/// Pitch rather than timbre because the binding's range is the measurement: a
/// controller value maps to cents linearly, so the reading names the value the
/// axis was reached with rather than only that it moved.
double folded_cents(const FoldedDimension& dimension, bool zoned, uint8_t manager_value,
                    uint8_t member_value, double expect_cents) {
  ControllerProfile profile;
  REQUIRE(
      profile.bind({dimension.input, dimension.index, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  if (zoned) send_mcm(synth, 0, 7);
  send(synth, dimension.step(0, manager_value));
  send(synth, dimension.step(2, member_value));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, expect_cents / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

Ump folded_wide_step(const FoldedDimension& dimension, uint8_t channel, uint32_t value) {
  return dimension.input == ControllerInput::kChannelPressure
             ? sonare::midi::make_midi2_channel_pressure(0, channel, value)
             : sonare::midi::make_midi2_control_change(0, channel, dimension.index, value);
}

std::vector<float> folded_wide_audio(const FoldedDimension& dimension, bool zoned, uint32_t value) {
  ControllerProfile profile;
  REQUIRE(
      profile.bind({dimension.input, dimension.index, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  if (zoned) send_mcm(synth, 0, 7);
  send(synth, folded_wide_step(dimension, 2, value));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  return render_left(synth, 24576);
}

std::vector<float> render_released_folded(const FoldedDimension& dimension, bool update_member,
                                          ControllerAxis axis = ControllerAxis::kPitchCents) {
  ControllerProfile profile;
  REQUIRE(profile.bind({dimension.input, dimension.index, axis, 0.0f,
                        axis == ControllerAxis::kPitchCents ? 1200.0f : 1.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, dimension.step(2, 20));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 64, 127));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (update_member) send(synth, dimension.step(2, 100));
  return render_left(synth, 8192);
}

std::vector<float> render_released_manager_folded(const FoldedDimension& dimension,
                                                  bool update_manager, ControllerAxis axis) {
  ControllerProfile profile;
  REQUIRE(profile.bind({dimension.input, dimension.index, axis, 0.0f,
                        axis == ControllerAxis::kPitchCents ? 1200.0f : 1.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, dimension.step(0, 20));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 64, 127));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (update_manager) send(synth, dimension.step(0, 100));
  return render_left(synth, 8192);
}

std::vector<float> render_held_folded(const FoldedDimension& dimension, bool update_member) {
  ControllerProfile profile;
  REQUIRE(
      profile.bind({dimension.input, dimension.index, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, dimension.step(2, 20));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  if (update_member) send(synth, dimension.step(2, 100));
  return render_left(synth, 8192);
}

std::vector<float> render_released_bend(bool update_member, bool update_manager) {
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 64, 127));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (update_member) {
    send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, 8192 - 2048));
  }
  if (update_manager) {
    send(synth, sonare::midi::make_midi1_pitch_bend(0, 0, kBendUp));
  }
  return render_left(synth, 8192);
}

std::vector<float> render_held_bend(bool update_member) {
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  if (update_member) {
    send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, 8192 - 2048));
  }
  return render_left(synth, 8192);
}

std::vector<float> render_released_channel_control(uint8_t controller, uint8_t value, bool update) {
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 64, 127));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (update) send(synth, sonare::midi::make_midi1_control_change(0, 2, controller, value));
  return render_left(synth, 8192);
}

double render_after_manager_reset(const FoldedDimension& dimension, uint8_t manager_value,
                                  uint8_t member_value, bool reset_manager, double expect_cents) {
  ControllerProfile profile;
  REQUIRE(
      profile.bind({dimension.input, dimension.index, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  if (member_value != 0) send(synth, dimension.step(2, member_value));
  if (manager_value != 0) send(synth, dimension.step(0, manager_value));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 64, 127));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  if (reset_manager) {
    send(synth, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
  }
  // Start a fresh voice after the reset so the assertion measures the retained
  // axis value rather than the phase history of the voice that saw the manager
  // gesture. The pitch estimate is insensitive to allocation age and envelope
  // phase, while a stale axis shifts it by hundreds of cents.
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 120, 0));
  render_left(synth, 1024);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, expect_cents / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

/// The dimension which wrote a shared profile axis last must be the one that
/// survives a manager reset.  The helper sends both manager values first so a
/// later member message is a genuinely folded value, then changes the order of
/// the two member writers.
double render_shared_axis_after_manager_reset(bool pressure_last) {
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  REQUIRE(profile.bind(
      {ControllerInput::kControlChange, kMpeTimbreCc, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);

  send(synth, pressure_step(0, 70));
  send(synth, timbre_step(0, 80));
  if (pressure_last) {
    send(synth, timbre_step(2, 20));
    send(synth, pressure_step(2, 30));
  } else {
    send(synth, pressure_step(2, 30));
    send(synth, timbre_step(2, 20));
  }
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
  // Remove the pre-reset voice before measuring a fresh allocation. This keeps
  // the assertion about the retained controller source independent of phase.
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 120, 0));
  render_left(synth, 1024);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double expected = 1200.0 * static_cast<double>(pressure_last ? 30 : 20) / 127.0;
  const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, expected / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

/// Manager first, member second: the member message carries the combined value,
/// and the reset must still reduce it to the member's own.
double render_manager_first_member_second_reset() {
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, pressure_step(0, 80));
  send(synth, pressure_step(2, 30));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 120, 0));
  render_left(synth, 1024);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double hz =
      fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, 1200.0 * 30.0 / 127.0 / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

double render_member_reset_manager_survival(bool reset_member, bool member_input = true) {
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, pressure_step(0, 80));
  if (member_input) send(synth, pressure_step(2, 30));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  if (reset_member) send(synth, sonare::midi::make_midi1_control_change(0, 2, 121, 0));
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 120, 0));
  render_left(synth, 1024);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double expected =
      1200.0 * static_cast<double>(reset_member || !member_input ? 80 : 110) / 127.0;
  const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, expected / 1200.0));
  return 1200.0 * std::log2(hz / kNoteHz);
}

struct SourceVoiceRender {
  std::vector<float> first;
  std::vector<float> second;
};

SourceVoiceRender render_source_voice_pair(NativeSynth& synth, int block = 8192) {
  std::vector<float> fallback_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(block), 0.0f);
  std::vector<float> first_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> first_r(static_cast<size_t>(block), 0.0f);
  std::vector<float> second_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> second_r(static_cast<size_t>(block), 0.0f);
  float* fallback_channels[] = {fallback_l.data(), fallback_r.data()};
  float* first_channels[] = {first_l.data(), first_r.data()};
  float* second_channels[] = {second_l.data(), second_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_channels}, {1, first_channels}, {2, second_channels}};
  REQUIRE(synth.process_source_tracks(outputs, 3, 2, block));
  return {std::move(first_l), std::move(second_l)};
}

/// A manager fold can be applied to one already sounding voice, then a later
/// note changes the current tracking answer. Reset must use the voice's source
/// history, retaining the member value on the original voice rather than
/// applying the post-reset attribution to the newer note.
SourceVoiceRender render_historical_voice_reset(bool manager_fold, bool reset_manager) {
  ControllerProfile profile;
  profile.pressure_tracking = sonare::midi::NoteTracking::kLastNote;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kExcitation, 0.0f, 1.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);

  // Keep the voice under test on source 1. Two distinct notes are required:
  // the MPE note tracker deduplicates repeated pitches, so a same-pitch pair
  // would never make the later note replace the historical attribution.
  MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 2, kNote - 12, kVelocity));
  first.source_track_id = 2;
  synth.on_event(0, first);
  render_left(synth, 2048);

  MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  second.source_track_id = 1;
  synth.on_event(0, second);
  render_left(synth, 2048);
  // The fold is applied to B (source 1), which is the current kLastNote.
  if (manager_fold) send(synth, pressure_step(0, 70));
  send(synth, pressure_step(2, 20));
  render_left(synth, 2048);
  // A third note changes the current kLastNote answer after the manager fold
  // has already been attributed to source 1.
  MidiEvent third = event(sonare::midi::make_midi1_note_on(0, 2, kNote + 12, kVelocity));
  third.source_track_id = 3;
  synth.on_event(0, third);
  render_left(synth, 2048);
  if (reset_manager) send(synth, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
  return render_source_voice_pair(synth, 16384);
}

SourceVoiceRender render_manager_zonewide_curve(bool manager_update) {
  ControllerProfile profile;
  profile.pressure_tracking = sonare::midi::NoteTracking::kLastNote;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kExcitation, 0.0f, 1.0f, 2.0f}));
  NativeSynth synth = make_synth(false);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  // The member raw value is a channel template copied by both notes. The
  // manager update must then reach both held voices and be composed before the
  // profile's nonlinear curve is evaluated.
  send(synth, pressure_step(2, 10));
  MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 2, kNote - 12, kVelocity));
  first.source_track_id = 1;
  synth.on_event(0, first);
  MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  second.source_track_id = 2;
  synth.on_event(0, second);
  render_left(synth, 4096);
  if (manager_update) send(synth, pressure_step(0, 90));
  return render_source_voice_pair(synth);
}

/// A per-note MPE engine axis must remain frozen through a member CC121 even
/// though the value was never stored in the channel-wide axis table.
std::vector<float> render_per_note_release_after_member_reset(bool reset_member,
                                                              bool apply_pressure = true) {
  ControllerProfile profile;
  profile.pressure_tracking = sonare::midi::NoteTracking::kLastNote;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  NativeSynth synth = make_synth(false);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 2, kNote - 12, kVelocity));
  first.source_track_id = 2;
  synth.on_event(0, first);
  MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  second.source_track_id = 1;
  synth.on_event(0, second);
  render_left(synth, 2048);
  if (apply_pressure) {
    send(synth, pressure_step(0, 70));
    send(synth, pressure_step(2, 20));
  }
  render_left(synth, 2048);
  MidiEvent note_off = event(sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  note_off.source_track_id = 1;
  synth.on_event(0, note_off);
  render_left(synth, 1024);
  if (reset_member) send(synth, sonare::midi::make_midi1_control_change(0, 2, 121, 0));
  return render_source_voice_pair(synth, 8192).first;
}

std::vector<float> render_released_ordinary_alias_reset(bool reset_member) {
  ControllerProfile profile;
  profile.pressure_tracking = sonare::midi::NoteTracking::kLastNote;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  REQUIRE(
      profile.bind({ControllerInput::kControlChange, 2, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, pressure_step(2, 20));
  // Ordinary CC2 is deliberately the last writer on the shared axis.
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 2, 110));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (reset_member) send(synth, sonare::midi::make_midi1_control_change(0, 2, 121, 0));
  return render_left(synth, 8192);
}

std::vector<float> render_released_bend_profile(bool update_member) {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kPitchBend, 0, ControllerAxis::kLoudness, 0.2f, 1.0f}));
  REQUIRE(
      profile.bind({ControllerInput::kPitchBend, 0, ControllerAxis::kPitchCents, -600.0f, 600.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  render_left(synth, 1024);
  if (update_member) {
    send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, 8192 - 2048));
  }
  return render_left(synth, 8192);
}

VelocityLaneRender render_velocity_lanes(ControllerAxis axis, uint16_t first_velocity,
                                         uint16_t second_velocity, bool add_second) {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kVelocity, 0, axis, 0.0f, 1.0f}));
  // Disable shared DC residuals to compare voice excitation independently.
  NativeSynth synth = make_synth(false);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);

  MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 2, kNote, first_velocity));
  first.source_track_id = 1;
  synth.on_event(0, first);
  const int block = 8192;
  std::vector<float> first_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> first_r(static_cast<size_t>(block), 0.0f);
  std::vector<float> second_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> second_r(static_cast<size_t>(block), 0.0f);
  std::vector<float> fallback_l(static_cast<size_t>(block), 0.0f);
  std::vector<float> fallback_r(static_cast<size_t>(block), 0.0f);
  float* first_channels[] = {first_l.data(), first_r.data()};
  float* second_channels[] = {second_l.data(), second_r.data()};
  float* fallback_channels[] = {fallback_l.data(), fallback_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_channels}, {1, first_channels}, {2, second_channels}};
  REQUIRE(synth.process_source_tracks(outputs, 3, 2, block));

  if (add_second) {
    MidiEvent second = event(
        sonare::midi::make_midi1_note_on(0, 2, static_cast<uint8_t>(kNote + 12), second_velocity));
    second.source_track_id = 2;
    synth.on_event(0, second);
  }
  std::fill(first_l.begin(), first_l.end(), 0.0f);
  std::fill(first_r.begin(), first_r.end(), 0.0f);
  std::fill(second_l.begin(), second_l.end(), 0.0f);
  std::fill(second_r.begin(), second_r.end(), 0.0f);
  std::fill(fallback_l.begin(), fallback_l.end(), 0.0f);
  std::fill(fallback_r.begin(), fallback_r.end(), 0.0f);
  REQUIRE(synth.process_source_tracks(outputs, 3, 2, block));
  return {std::move(first_l), std::move(second_l)};
}

std::vector<float> render_velocity_note(const Ump& note_on) {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kVelocity, 0, ControllerAxis::kExcitation, 0.0f, 1.0f}));
  NativeSynth synth = make_synth();
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send(synth, note_on);
  return render_left(synth, 8192);
}

/// Two notes far enough apart that neither sits on a harmonic of the other,
/// before or after a bend of kAttributionSemitones: 130.81 -> 196.00 and
/// 739.99 -> 1108.73, with no fundamental landing within a bin of another's.
constexpr uint8_t kLowNote = 48;
constexpr uint8_t kHighNote = 78;
constexpr double kLowHz = 130.8128;
constexpr double kHighHz = 739.9888;
/// A member channel bends +/-48 semitones, so this asks for +7.
constexpr uint16_t kAttributionBend = 9386;

/// Power in the fundamental of @p hz, which is present while the note sounds
/// where it started and gone once a bend has moved it.
double fundamental_power(const std::vector<float>& audio, double hz) {
  return sonare::test::harmonic_power(sonare::test::power_spectrum(audio, 4096), hz, 1);
}

/// Both notes started on one member channel, then one bend addressed to that
/// channel -- the case 2.2.4.1 declines to answer and the profile's tracking
/// rule does.
std::vector<float> render_two_notes(sonare::midi::NoteTracking tracking) {
  ControllerProfile profile;
  profile.bend_tracking = tracking;
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  NativeSynth synth(cfg);
  synth.set_controller_profile(profile);
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kLowNote, kVelocity));
  render_left(synth, 2048);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kHighNote, kVelocity));
  render_left(synth, 2048);
  send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, kAttributionBend));
  return render_left(synth, 24576);
}

}  // namespace

TEST_CASE("a bend on a member channel moves the note it was attributed to", "[midi][synth][mpe]") {
  // Two notes on one member channel is the degraded form of MPE, and the
  // specification hands the answer back: "When there is more than one
  // concurrent Active Note on a Member Channel, implementation of how
  // controllers affect the notes is up to the Device" (2.2.4.1). The profile's
  // tracking rule is where the device's answer is written down, so the same
  // messages have to render differently under two of them.
  const std::vector<float> lowest = render_two_notes(sonare::midi::NoteTracking::kLowestNote);
  const std::vector<float> highest = render_two_notes(sonare::midi::NoteTracking::kHighestNote);
  const std::vector<float> all = render_two_notes(sonare::midi::NoteTracking::kAllNotes);

  const double low_under_lowest = fundamental_power(lowest, kLowHz);
  const double low_under_highest = fundamental_power(highest, kLowHz);
  const double high_under_lowest = fundamental_power(lowest, kHighHz);
  const double high_under_highest = fundamental_power(highest, kHighHz);
  CAPTURE(low_under_lowest, low_under_highest, high_under_lowest, high_under_highest);

  // The note the rule names leaves its starting pitch; the other one stays on
  // it. Both directions, so an implementation that bent every note or none of
  // them fails whichever way it erred.
  REQUIRE(low_under_lowest * 4.0 < low_under_highest);
  REQUIRE(high_under_highest * 4.0 < high_under_lowest);

  // kAllNotes is the rule under which the ambiguity does not arise, and it is
  // the one arm where both notes move.
  CAPTURE(fundamental_power(all, kLowHz), fundamental_power(all, kHighHz));
  REQUIRE(fundamental_power(all, kLowHz) * 4.0 < low_under_highest);
  REQUIRE(fundamental_power(all, kHighHz) * 4.0 < high_under_lowest);
}

TEST_CASE("a manager's value reaches a member's axis through the profile", "[midi][synth][mpe]") {
  // The two per-note dimensions that combine inside the controller domain reach
  // an axis through the controller profile rather than through the synth, so the
  // fold has to happen on the way in: a manager's value is a bias on every
  // member of its zone (2.2.7, 2.2.8) and Appendix D adds the two.
  //
  // The binding maps 0..127 onto 0..1200 cents, so 40 is 378 and 80 is 756.
  for (const FoldedDimension& dimension : kFolded) {
    CAPTURE(dimension.label);
    const double member_only = folded_cents(dimension, true, 0, 40, 378.0);
    const double manager_only = folded_cents(dimension, true, 40, 0, 378.0);
    const double both = folded_cents(dimension, true, 40, 40, 756.0);
    const double unzoned = folded_cents(dimension, false, 40, 0, 0.0);
    CAPTURE(member_only, manager_only, both, unzoned);

    // Sent to the member, which is the arm that would pass without any fold at
    // all -- it is here so the three below are read against a working binding.
    REQUIRE(std::fabs(member_only - 378.0) < 60.0);
    // Sent to the manager and nowhere else, and it still reaches the note.
    REQUIRE(std::fabs(manager_only - 378.0) < 60.0);
    REQUIRE(std::fabs(manager_only - member_only) < 30.0);
    // Sent to both, and they add rather than one replacing the other.
    REQUIRE(std::fabs(both - 756.0) < 60.0);
    // Outside a zone the manager channel is an ordinary channel: the same
    // message reaches channel 2 not at all.
    REQUIRE(std::fabs(unzoned) < 30.0);
  }
}

TEST_CASE("an MCM replaces the channel's bend range with the zone's", "[midi][synth][mpe]") {
  // The same bend message, the same patch, the same note. Only the MCM differs,
  // and the two ranges it chooses between are 2 semitones and 48.
  NativeSynth plain = make_synth();
  plain.prepare(kRate, kBlock);
  const double without_zone = bent_cents(plain, 2, kBendUp, 50.0);

  NativeSynth zoned = make_synth();
  zoned.prepare(kRate, kBlock);
  send_mcm(zoned, 0, 7);
  const double with_zone = bent_cents(zoned, 2, kBendUp, 1200.0);

  CAPTURE(without_zone, with_zone);
  // A quarter-scale bend is +50 cents at the ordinary range and +1200 at the
  // member range the MCM installs. Generous bounds, because the reed's own
  // tuning residual rides on both and the ratio is what carries the claim.
  REQUIRE(std::fabs(without_zone - 50.0) < 40.0);
  REQUIRE(std::fabs(with_zone - 1200.0) < 60.0);
}

TEST_CASE("a manager channel's bend reaches a note on a member", "[midi][synth][mpe]") {
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);

  // Nothing is sent to channel 2 at all: the bend goes to the manager, and the
  // note's pitch has to move anyway. The manager keeps the ordinary 2-semitone
  // range rather than the member's 48, so the size of the move says which of
  // the two sensitivities was read.
  send(synth, sonare::midi::make_midi1_pitch_bend(0, 0, kBendUp));
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  const std::vector<float> audio = render_left(synth, 24576);
  const double cents = 1200.0 * std::log2(fft_fundamental(audio, 8192, kNoteHz * 1.2) / kNoteHz);
  CAPTURE(cents);
  REQUIRE(std::fabs(cents - 50.0) < 40.0);
  // And it is not the member range: 1200 cents would be an octave away and
  // outside any tolerance this case could carry.
  REQUIRE(cents < 600.0);
}

TEST_CASE("reconfiguring the zone silences the channels that left it", "[midi][synth][mpe]") {
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);

  auto peak_of = [](const std::vector<float>& audio) {
    float peak = 0.0f;
    for (const float s : audio) peak = std::max(peak, std::fabs(s));
    return peak;
  };

  // One note per arm so the two are never summed: channel 5 leaves the zone
  // when it shrinks to two members, channel 2 stays inside it. The pair is what
  // says the stop follows the channels that moved rather than silencing
  // everything, which would pass the first half on its own.
  auto reconfigure_with_note_on = [&](uint8_t channel) {
    NativeSynth arm = make_synth();
    arm.prepare(kRate, kBlock);
    send_mcm(arm, 0, 7);
    send(arm, sonare::midi::make_midi1_note_on(0, channel, kNote, kVelocity));
    const float before = peak_of(render_left(arm, 8192));
    REQUIRE(before > 0.0f);
    send_mcm(arm, 0, 2);
    return peak_of(render_left(arm, 8192)) / before;
  };

  const float left_zone = reconfigure_with_note_on(5);
  const float stayed = reconfigure_with_note_on(2);
  CAPTURE(left_zone, stayed);
  // The note on the channel that left is stopped outright rather than released,
  // because 2.2.3 asks the receiver to stop sounding notes on it.
  REQUIRE(left_zone < 0.01f);
  REQUIRE(stayed > 0.5f);

  // A note started after the reconfiguration sounds normally, so the silence
  // above is the old note being stopped rather than the synth being wedged.
  send_mcm(synth, 0, 2);
  send(synth, sonare::midi::make_midi1_note_on(0, 1, kNote, kVelocity));
  REQUIRE(peak_of(render_left(synth, 8192)) > 0.0f);
}

TEST_CASE("a program change is ignored on a member channel in Mode 3", "[midi][synth][mpe]") {
  // A zone is monotimbral in Mode 3, so the program belongs to the manager and
  // a member channel's program change is dropped (2.3.3). Observed as sound
  // rather than as state: a cello and the default piano share no timbre, so a
  // program change that took is audible and one that was dropped leaves the
  // render identical to the one where it was never sent.
  auto render_after_program = [](uint8_t channel, bool send_program, bool mono_mode) {
    NativeSynthConfig cfg;
    cfg.patch = NativeSynthPatch{};
    cfg.use_gm_programs = true;
    NativeSynth synth(cfg);
    synth.prepare(kRate, kBlock);
    send_mcm(synth, 0, 7);
    if (mono_mode) send(synth, sonare::midi::make_midi1_control_change(0, channel, 126, 0));
    if (send_program) send(synth, sonare::midi::make_midi1_program_change(0, channel, 42));
    send(synth, sonare::midi::make_midi1_note_on(0, channel, kNote, kVelocity));
    return render_left(synth, 12288);
  };

  const std::vector<float> member_untouched = render_after_program(2, false, false);
  const std::vector<float> member_asked = render_after_program(2, true, false);
  const std::vector<float> outside_asked = render_after_program(10, true, false);
  const std::vector<float> outside_untouched = render_after_program(10, false, false);

  // Refused on the member: asking changed nothing at all.
  REQUIRE(member_asked == member_untouched);
  // Accepted outside the zone, which is what says the message is one this synth
  // acts on rather than one it drops everywhere.
  REQUIRE(outside_asked != outside_untouched);

  // Mode 4 is the case the spec permits it in, and a controller reaches it by
  // sending CC#126 to a member channel.
  REQUIRE(render_after_program(2, true, true) != member_untouched);
}

TEST_CASE("a mode message sent to the manager channel is ignored", "[midi][synth][mpe]") {
  // CC#126 and #127 are prohibited on a manager channel, and they carry an
  // all-notes-off everywhere else -- so a receiver that took them there would
  // silence the whole zone on a message it was required to drop.
  NativeSynth synth = make_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);

  send(synth, sonare::midi::make_midi1_control_change(0, 0, 126, 0));
  const std::vector<float> after = render_left(synth, 8192);
  float peak = 0.0f;
  for (const float s : after) peak = std::max(peak, std::fabs(s));
  REQUIRE(peak > 0.0f);

  // The same message on a member channel is the one that is accepted, and it
  // does carry the all-notes-off, so the case above is a refusal rather than a
  // synth that ignores CC#126 outright.
  send(synth, sonare::midi::make_midi1_control_change(0, 2, 126, 0));
  const std::vector<float> silenced = render_left(synth, 8192);
  float silenced_peak = 0.0f;
  for (const float s : silenced) silenced_peak = std::max(silenced_peak, std::fabs(s));
  CAPTURE(peak, silenced_peak);
  REQUIRE(silenced_peak < peak);
}

TEST_CASE("an MCM sent as a MIDI 2.0 Registered Controller configures the zone",
          "[midi][synth][mpe]") {
  // RPN 00 06 in its MIDI 2.0 form: one Registered Controller message whose
  // data MSB is the member count, the same gesture as the three CCs above.
  NativeSynth zoned = make_synth();
  zoned.prepare(kRate, kBlock);
  send(zoned, sonare::midi::make_midi2_registered_controller(0, 0, 0, 6, uint32_t{7} << 25));
  const double with_zone = bent_cents(zoned, 2, kBendUp, 1200.0);
  CAPTURE(with_zone);
  REQUIRE(std::fabs(with_zone - 1200.0) < 60.0);
}

TEST_CASE("MPE profile forwarding preserves MIDI 2.0 pressure and timbre width",
          "[midi][synth][mpe][midi2]") {
  // This value is between two MIDI 1.0 upscale points. A member-only fold has
  // no arithmetic to perform, so an MPE render must be exactly the same as an
  // ordinary channel receiving that same MIDI 2.0 message.
  const uint32_t raw = sonare::midi::scale_cc_7_to_32(64) + (uint32_t{1} << 24);
  for (const FoldedDimension& dimension : kFolded) {
    CAPTURE(dimension.label, raw);
    const std::vector<float> ordinary = folded_wide_audio(dimension, false, raw);
    const std::vector<float> mpe = folded_wide_audio(dimension, true, raw);
    REQUIRE(mpe == ordinary);
  }
}

TEST_CASE("MPE member dimensions keep updating while a key is held", "[midi][synth][mpe]") {
  for (const FoldedDimension& dimension : kFolded) {
    CAPTURE(dimension.label);
    const std::vector<float> unchanged = render_held_folded(dimension, false);
    const std::vector<float> changed = render_held_folded(dimension, true);
    REQUIRE(changed != unchanged);
  }

  const std::vector<float> bend_unchanged = render_held_bend(false);
  const std::vector<float> bend_held = render_held_bend(true);
  REQUIRE(bend_held != bend_unchanged);
}

TEST_CASE("MPE member dimensions freeze after Note Off but ordinary controls remain live",
          "[midi][synth][mpe]") {
  for (const FoldedDimension& dimension : kFolded) {
    CAPTURE(dimension.label);
    const std::vector<float> unchanged = render_released_folded(dimension, false);
    const std::vector<float> changed = render_released_folded(dimension, true);
    REQUIRE(changed == unchanged);

    // Engine-owned wind axes obey the same post-NoteOff freeze as channel
    // pitch. Both the member's own value and the manager bias are frozen.
    const std::vector<float> engine_unchanged =
        render_released_folded(dimension, false, ControllerAxis::kBrightness);
    const std::vector<float> engine_changed =
        render_released_folded(dimension, true, ControllerAxis::kBrightness);
    REQUIRE(engine_changed == engine_unchanged);
    const std::vector<float> manager_unchanged =
        render_released_manager_folded(dimension, false, ControllerAxis::kPitchCents);
    const std::vector<float> manager_changed =
        render_released_manager_folded(dimension, true, ControllerAxis::kPitchCents);
    REQUIRE(manager_changed == manager_unchanged);
    const std::vector<float> manager_engine_unchanged =
        render_released_manager_folded(dimension, false, ControllerAxis::kBrightness);
    const std::vector<float> manager_engine_changed =
        render_released_manager_folded(dimension, true, ControllerAxis::kBrightness);
    REQUIRE(manager_engine_changed == manager_engine_unchanged);
  }

  // Member bend is frozen with the same rule. Manager bend is the exception:
  // it remains a zone-wide pitch control for a sustain-held sounding voice.
  const std::vector<float> member_unchanged = render_released_bend(false, false);
  const std::vector<float> member_changed = render_released_bend(true, false);
  const std::vector<float> manager_changed = render_released_bend(false, true);
  REQUIRE(member_changed == member_unchanged);
  REQUIRE(manager_changed != member_unchanged);

  // Gain and pan are channel controls rather than MPE dimensions and must keep
  // affecting a sustain-held release tail.
  REQUIRE(render_released_channel_control(7, 20, true) !=
          render_released_channel_control(7, 20, false));
  REQUIRE(render_released_channel_control(10, 0, true) !=
          render_released_channel_control(10, 0, false));
}

TEST_CASE("a member voice choked by its exclusive group freezes like a Note Off",
          "[midi][synth][mpe]") {
  const auto render_choked = [](bool bend_after) {
    NativeSynthConfig cfg;
    cfg.patch = NativeSynthPatch{};
    cfg.patch.cutoff_hz = 20000.0f;
    cfg.patch.amp_env.sustain = 1.0f;
    cfg.patch.amp_env.release_ms = 3000.0f;
    cfg.patch.percussion.exclusive_class = 1;
    NativeSynth synth(cfg);
    synth.prepare(kRate, kBlock);
    send_mcm(synth, 0, 7);
    send(synth, sonare::midi::make_midi1_note_on(0, 2, kLowNote, kVelocity));
    render_left(synth, 2048);
    // The second strike chokes the first; its own Note Off then freezes it.
    send(synth, sonare::midi::make_midi1_note_on(0, 2, kHighNote, kVelocity));
    render_left(synth, 256);
    send(synth, sonare::midi::make_midi1_note_off(0, 2, kHighNote, 0));
    REQUIRE(synth.active_voice_count() == 2);
    if (bend_after) send(synth, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
    return render_left(synth, 8192);
  };
  CHECK(render_choked(true) == render_choked(false));
}

TEST_CASE("manager Reset All Controllers clears only manager-derived MPE axes",
          "[midi][synth][mpe]") {
  for (const FoldedDimension& dimension : kFolded) {
    CAPTURE(dimension.label);
    // A manager-only value must not remain latched on a member after CC121.
    const double manager_reset = render_after_manager_reset(dimension, 90, 0, true, 0.0);
    const double fresh = render_after_manager_reset(dimension, 0, 0, false, 0.0);
    CAPTURE(manager_reset, fresh);
    REQUIRE(std::fabs(manager_reset - fresh) < 60.0);

    // A member's own value is independent state and survives a manager reset.
    const double member_reset =
        render_after_manager_reset(dimension, 90, 30, true, 1200.0 * 30.0 / 127.0);
    const double member_only =
        render_after_manager_reset(dimension, 0, 30, false, 1200.0 * 30.0 / 127.0);
    CAPTURE(member_reset, member_only);
    REQUIRE(std::fabs(member_reset - member_only) < 60.0);
  }
}

TEST_CASE("manager reset preserves the last writer on a shared MPE axis", "[midi][synth][mpe]") {
  const double pressure_last = render_shared_axis_after_manager_reset(true);
  const double timbre_last = render_shared_axis_after_manager_reset(false);
  CAPTURE(pressure_last, timbre_last);
  REQUIRE(std::fabs(pressure_last - 1200.0 * 30.0 / 127.0) < 60.0);
  REQUIRE(std::fabs(timbre_last - 1200.0 * 20.0 / 127.0) < 60.0);
}

TEST_CASE("manager-first then member-second reset retains the member value", "[midi][synth][mpe]") {
  const double after_reset = render_manager_first_member_second_reset();
  CAPTURE(after_reset);
  REQUIRE(std::fabs(after_reset - 1200.0 * 30.0 / 127.0) < 60.0);
}

TEST_CASE("resetting a member keeps the manager's surviving MPE bias", "[midi][synth][mpe]") {
  const double reset_member = render_member_reset_manager_survival(true);
  const double manager_only = render_member_reset_manager_survival(false, false);
  const double combined = render_member_reset_manager_survival(false);
  CAPTURE(reset_member, manager_only);
  REQUIRE(std::fabs(reset_member - manager_only) < 60.0);
  REQUIRE(std::fabs(reset_member - 1200.0 * 80.0 / 127.0) < 60.0);
  REQUIRE(combined - manager_only > 200.0);
}

TEST_CASE("manager reset uses historical attribution for a voice-only fold", "[midi][synth][mpe]") {
  const SourceVoiceRender expected = render_historical_voice_reset(false, false);
  const SourceVoiceRender stale = render_historical_voice_reset(true, false);
  const SourceVoiceRender after_reset = render_historical_voice_reset(true, true);
  const float expected_rms = sonare::test::rms(expected.first);
  const float stale_rms = sonare::test::rms(stale.first);
  const float reset_rms = sonare::test::rms(after_reset.first);
  CAPTURE(expected_rms, stale_rms, reset_rms);
  // Source 1 is the historical target of the manager+member fold. Source 3
  // arrives afterwards and changes the current kLastNote answer, so a reset
  // that re-runs attribution drops source 1's member value. The reset result
  // must return to the member-only source-1 render, while the stale combined
  // render remains measurably different.
  REQUIRE(std::fabs(reset_rms - expected_rms) < 1.0e-3f);
  REQUIRE(std::fabs(stale_rms - expected_rms) > 1.0e-3f);
}

TEST_CASE("manager MPE pressure reaches every held member voice before nonlinear mapping",
          "[midi][synth][mpe]") {
  const SourceVoiceRender without_manager = render_manager_zonewide_curve(false);
  const SourceVoiceRender with_manager = render_manager_zonewide_curve(true);
  const float first_without = sonare::test::rms(without_manager.first);
  const float second_without = sonare::test::rms(without_manager.second);
  const float first_with = sonare::test::rms(with_manager.first);
  const float second_with = sonare::test::rms(with_manager.second);
  CAPTURE(first_without, first_with, second_without, second_with);
  // Both voices are the same patch and both carry the member template. A
  // manager-only update is zone-wide; routing it only to kLastNote leaves one
  // lane at its old target and loses the nonlinear pre-curve composition.
  REQUIRE(std::fabs(first_with - first_without) > 1.0e-3f);
  REQUIRE(std::fabs(second_with - second_without) > 1.0e-3f);
}

TEST_CASE("a per-note MPE excitation snapshot survives member reset after Note Off",
          "[midi][synth][mpe]") {
  const std::vector<float> without_reset = render_per_note_release_after_member_reset(false, true);
  const std::vector<float> with_reset = render_per_note_release_after_member_reset(true, true);
  const std::vector<float> without_pressure =
      render_per_note_release_after_member_reset(false, false);
  float max_delta = 0.0f;
  for (size_t i = 0; i < without_reset.size(); ++i) {
    max_delta = std::max(max_delta, std::fabs(without_reset[i] - with_reset[i]));
  }
  float pressure_delta = 0.0f;
  for (size_t i = 0; i < without_reset.size(); ++i) {
    pressure_delta = std::max(pressure_delta, std::fabs(without_reset[i] - without_pressure[i]));
  }
  CAPTURE(max_delta, pressure_delta, sonare::test::rms(without_reset),
          sonare::test::rms(with_reset), sonare::test::rms(without_pressure));
  REQUIRE(max_delta < 1.0e-6f);
  REQUIRE(pressure_delta > 1.0e-5f);
}

TEST_CASE("member reset removes a released ordinary alias but keeps MPE state",
          "[midi][synth][mpe]") {
  const std::vector<float> without_reset = render_released_ordinary_alias_reset(false);
  const std::vector<float> with_reset = render_released_ordinary_alias_reset(true);
  float max_delta = 0.0f;
  for (size_t i = 0; i < without_reset.size(); ++i) {
    max_delta = std::max(max_delta, std::fabs(without_reset[i] - with_reset[i]));
  }
  CAPTURE(max_delta, sonare::test::rms(without_reset), sonare::test::rms(with_reset));
  REQUIRE(max_delta > 1.0e-5f);
}

TEST_CASE("member bend profile axes freeze after Note Off", "[midi][synth][mpe]") {
  const std::vector<float> unchanged = render_released_bend_profile(false);
  const std::vector<float> changed = render_released_bend_profile(true);
  float max_delta = 0.0f;
  for (size_t i = 0; i < unchanged.size(); ++i) {
    max_delta = std::max(max_delta, std::fabs(unchanged[i] - changed[i]));
  }
  CAPTURE(max_delta, sonare::test::rms(unchanged), sonare::test::rms(changed));
  REQUIRE(max_delta < 1.0e-6f);
}

TEST_CASE("velocity excitation is seeded on the new wind voice only", "[midi][synth][wind][mpe]") {
  const VelocityLaneRender with_second =
      render_velocity_lanes(ControllerAxis::kExcitation, 20, 110, true);
  const VelocityLaneRender without_second =
      render_velocity_lanes(ControllerAxis::kExcitation, 20, 110, false);
  CAPTURE(sonare::test::rms(with_second.second), sonare::test::rms(without_second.first));
  REQUIRE(sonare::test::rms(without_second.first) > 1.0e-6f);
  REQUIRE(sonare::test::rms(with_second.second) > 1.0e-6f);
  // Track 1's held note remains bit-identical when track 2 arrives with a
  // different velocity. Track 2 still receives its own seeded excursion.
  REQUIRE(with_second.first == without_second.first);
}

TEST_CASE("velocity loudness remains an accepted channel-wide control", "[midi][synth][wind]") {
  const VelocityLaneRender with_second =
      render_velocity_lanes(ControllerAxis::kLoudness, 20, 110, true);
  const VelocityLaneRender without_second =
      render_velocity_lanes(ControllerAxis::kLoudness, 20, 110, false);
  // Existing channel-level velocity bindings intentionally continue to affect
  // the already sounding channel when the next note supplies a new value.
  REQUIRE(with_second.first != without_second.first);
}

TEST_CASE("velocity excitation reaches a mono-legato continuation", "[midi][synth][wind]") {
  const auto render_slur = [](uint8_t second_velocity) {
    ControllerProfile profile;
    REQUIRE(profile.bind({ControllerInput::kVelocity, 0, ControllerAxis::kExcitation, 0.0f, 1.0f}));
    NativeSynth synth = make_synth(false);
    synth.set_controller_profile(profile);
    synth.prepare(kRate, kBlock);
    REQUIRE(synth.set_articulation(2, sonare::midi::ArticulationMode::kMonoLegato));
    send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, 20));
    render_left(synth, 4096);
    send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote + 2, second_velocity));
    std::vector<float> slurred = render_left(synth, 8192);
    uint64_t fallbacks = 0;
    REQUIRE(synth.legato_fallback_count(&fallbacks));
    REQUIRE(fallbacks == 0);
    REQUIRE(synth.active_voice_count() == 1);
    return slurred;
  };
  const std::vector<float> soft = render_slur(20);
  const std::vector<float> hard = render_slur(120);
  CAPTURE(sonare::test::rms(soft), sonare::test::rms(hard));
  CHECK(soft != hard);
}

TEST_CASE("velocity excitation keeps MIDI 2.0 midpoint resolution", "[midi][synth][wind][midi2]") {
  const uint16_t midpoint = sonare::midi::scale_velocity_7_to_16(64) + 0x0100u;
  const std::vector<float> lower =
      render_velocity_note(sonare::midi::make_midi1_note_on(0, 2, kNote, 64));
  const std::vector<float> upper =
      render_velocity_note(sonare::midi::make_midi1_note_on(0, 2, kNote, 65));
  const std::vector<float> wide =
      render_velocity_note(sonare::midi::make_midi2_note_on(0, 2, kNote, midpoint));
  REQUIRE(wide != lower);
  REQUIRE(wide != upper);
}

namespace {

/// A fast-releasing subtractive synth, so a note that is not held ends within the render.
NativeSynth make_fast_release_synth() {
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.mode = SynthEngineMode::kSubtractive;
  cfg.patch.amp_env.attack_ms = 1.0f;
  cfg.patch.amp_env.decay_ms = 1.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 20.0f;
  cfg.dc_block = false;
  return NativeSynth(cfg);
}

/// Voices still alive 0.5 s after a note-off on @p note_channel, with @p pedal_cc down on
/// @p pedal_channel before the note-off (-1 for no pedal).
int voices_after_pedalled_note_off(uint8_t pedal_cc, int pedal_channel, uint8_t note_channel,
                                   bool zoned, bool pedal_up_again) {
  NativeSynth synth = make_fast_release_synth();
  synth.prepare(kRate, kBlock);
  if (zoned) send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_note_on(0, note_channel, kNote, kVelocity));
  render_left(synth, 4096);
  if (pedal_channel >= 0) {
    send(synth, sonare::midi::make_midi1_control_change(0, static_cast<uint8_t>(pedal_channel),
                                                        pedal_cc, 127));
  }
  send(synth, sonare::midi::make_midi1_note_off(0, note_channel, kNote, 0));
  render_left(synth, 24000);
  if (pedal_up_again && pedal_channel >= 0) {
    send(synth, sonare::midi::make_midi1_control_change(0, static_cast<uint8_t>(pedal_channel),
                                                        pedal_cc, 0));
    render_left(synth, 24000);
  }
  return synth.active_voice_count();
}

}  // namespace

TEST_CASE("a manager channel's pedal holds the notes of its whole zone", "[midi][synth][mpe]") {
  // CC64 only: sustain. CC66 is exercised with the key down at the pedal edge.
  CHECK(voices_after_pedalled_note_off(64, 0, 2, true, false) == 1);
  CHECK(voices_after_pedalled_note_off(64, 0, 2, true, true) == 0);
  // The manager's own notes and the other zone's channels.
  CHECK(voices_after_pedalled_note_off(64, 0, 0, true, false) == 1);
  CHECK(voices_after_pedalled_note_off(64, 0, 12, true, false) == 0);
  // The upper zone's manager reaches its members (channel 14), not the lower zone's.
  {
    NativeSynth synth = make_fast_release_synth();
    synth.prepare(kRate, kBlock);
    send_mcm(synth, 15, 7);
    send(synth, sonare::midi::make_midi1_note_on(0, 14, kNote, kVelocity));
    render_left(synth, 4096);
    send(synth, sonare::midi::make_midi1_control_change(0, 15, 64, 127));
    send(synth, sonare::midi::make_midi1_note_off(0, 14, kNote, 0));
    render_left(synth, 24000);
    CHECK(synth.active_voice_count() == 1);
  }
  // Without a zone, a pedal on channel 0 is channel 0's alone.
  CHECK(voices_after_pedalled_note_off(64, 0, 2, false, false) == 0);
  CHECK(voices_after_pedalled_note_off(64, 0, 0, false, false) == 1);
  // A member's own pedal stays on the member.
  CHECK(voices_after_pedalled_note_off(64, 2, 2, true, false) == 1);
  CHECK(voices_after_pedalled_note_off(64, 3, 2, true, false) == 0);
}

TEST_CASE("a manager channel's sostenuto captures only the keys down at the pedal edge",
          "[midi][synth][mpe]") {
  NativeSynth synth = make_fast_release_synth();
  synth.prepare(kRate, kBlock);
  send_mcm(synth, 0, 7);
  send(synth, sonare::midi::make_midi1_note_on(0, 2, kNote, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_control_change(0, 0, 66, 127));
  // A key struck after the edge is not captured.
  send(synth, sonare::midi::make_midi1_note_on(0, 3, kNote + 2, kVelocity));
  render_left(synth, 4096);
  send(synth, sonare::midi::make_midi1_note_off(0, 2, kNote, 0));
  send(synth, sonare::midi::make_midi1_note_off(0, 3, kNote + 2, 0));
  render_left(synth, 24000);
  CHECK(synth.active_voice_count() == 1);
  send(synth, sonare::midi::make_midi1_control_change(0, 0, 66, 0));
  render_left(synth, 24000);
  CHECK(synth.active_voice_count() == 0);
}

TEST_CASE("CC126 and CC127 outside a zone select the channel's articulation",
          "[midi][synth][mpe]") {
  using sonare::midi::ArticulationMode;
  auto voices_after_overlap = [](NativeSynth& synth) {
    send(synth, sonare::midi::make_midi1_note_on(0, 1, 60, kVelocity));
    render_left(synth, 4096);
    send(synth, sonare::midi::make_midi1_note_on(0, 1, 64, kVelocity));
    render_left(synth, 24000);
    return synth.active_voice_count();
  };
  ArticulationMode mode = ArticulationMode::kPoly;

  NativeSynth mono = make_fast_release_synth();
  mono.prepare(kRate, kBlock);
  send(mono, sonare::midi::make_midi1_control_change(0, 1, 126, 1));
  REQUIRE(mono.articulation(1, &mode));
  CHECK(mode == ArticulationMode::kMonoRetrigger);
  CHECK(voices_after_overlap(mono) == 1);
  REQUIRE(mono.articulation(0, &mode));
  CHECK(mode == ArticulationMode::kPoly);

  NativeSynth direct = make_fast_release_synth();
  direct.prepare(kRate, kBlock);
  REQUIRE(direct.set_articulation(1, ArticulationMode::kMonoRetrigger));
  CHECK(voices_after_overlap(direct) == 1);

  // Back to polyphony: both overlapping notes sound.
  send(mono, sonare::midi::make_midi1_control_change(0, 1, 127, 0));
  REQUIRE(mono.articulation(1, &mode));
  CHECK(mode == ArticulationMode::kPoly);
  NativeSynth poly = make_fast_release_synth();
  poly.prepare(kRate, kBlock);
  send(poly, sonare::midi::make_midi1_control_change(0, 1, 126, 1));
  send(poly, sonare::midi::make_midi1_control_change(0, 1, 127, 0));
  CHECK(voices_after_overlap(poly) == 2);

  // Inside a zone a member's mode message keeps its MPE meaning and leaves the articulation.
  NativeSynth zoned = make_fast_release_synth();
  zoned.prepare(kRate, kBlock);
  send_mcm(zoned, 0, 7);
  send(zoned, sonare::midi::make_midi1_control_change(0, 2, 126, 1));
  REQUIRE(zoned.articulation(2, &mode));
  CHECK(mode == ArticulationMode::kPoly);
}

TEST_CASE("All Sound Off on another channel leaves a shared piano body ringing",
          "[midi][synth][mpe]") {
  for (const double rate : {48000.0, 96000.0}) {
    auto make = [&](int cc_channel) {
      NativeSynthConfig cfg;
      cfg.patch = NativeSynthPatch{};
      cfg.patch.mode = SynthEngineMode::kPiano;
      cfg.patch.amp_env.release_ms = 5.0f;
      NativeSynth synth(cfg);
      synth.prepare(rate, kBlock);
      send(synth, sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity));
      render_left(synth, static_cast<int>(rate * 0.3));
      send(synth, sonare::midi::make_midi1_note_off(0, 0, kNote, 0));
      for (int i = 0; i < 400 && synth.active_voice_count() > 0; ++i) render_left(synth, kBlock);
      REQUIRE(synth.active_voice_count() == 0);
      if (cc_channel >= 0) {
        send(synth,
             sonare::midi::make_midi1_control_change(0, static_cast<uint8_t>(cc_channel), 120, 0));
      }
      return render_left(synth, 4096);
    };
    const std::vector<float> control = make(-1);
    const std::vector<float> other = make(1);
    const std::vector<float> owner = make(0);
    float control_peak = 0.0f;
    float owner_peak = 0.0f;
    for (const float s : control) control_peak = std::max(control_peak, std::fabs(s));
    for (const float s : owner) owner_peak = std::max(owner_peak, std::fabs(s));
    CAPTURE(rate, control_peak, owner_peak);
    REQUIRE(control_peak > 1.0e-6f);
    CHECK(other == control);
    CHECK(owner_peak < control_peak * 0.01f);
  }
}

TEST_CASE("a manager's pressure and reset reach exactly the members of its zone",
          "[midi][synth][mpe]") {
  ControllerProfile profile;
  REQUIRE(profile.bind(
      {ControllerInput::kChannelPressure, 0, ControllerAxis::kPitchCents, 0.0f, 1200.0f}));
  // Cents of a note on @p channel after the lower manager sends pressure 127, optionally followed
  // by that manager's Reset All Controllers.
  auto cents_on = [&](uint8_t channel, bool reset) {
    NativeSynth synth = make_synth();
    synth.set_controller_profile(profile);
    synth.prepare(kRate, kBlock);
    send_mcm(synth, 0, 3);
    send_mcm(synth, 15, 3);
    send(synth, sonare::midi::make_midi1_channel_pressure(0, 0, 127));
    send(synth, sonare::midi::make_midi1_note_on(0, channel, kNote, kVelocity));
    if (reset) send(synth, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
    const std::vector<float> audio = render_left(synth, 24576);
    const double hz = fft_fundamental(audio, 8192, kNoteHz * std::pow(2.0, 1.0));
    return 1200.0 * std::log2(hz / kNoteHz);
  };
  const double lower_member = cents_on(2, false);
  const double upper_member = cents_on(13, false);
  const double unassigned = cents_on(6, false);
  CAPTURE(lower_member, upper_member, unassigned);
  CHECK(lower_member > 600.0);
  CHECK(std::fabs(upper_member) < 50.0);
  CHECK(std::fabs(unassigned) < 50.0);
  // After the manager's reset its bias is gone from its own members only.
  CHECK(std::fabs(cents_on(2, true)) < 50.0);
}
