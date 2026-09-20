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

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

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

NativeSynth make_synth() {
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.mode = SynthEngineMode::kReed;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.amp_env.sustain = 1.0f;
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
