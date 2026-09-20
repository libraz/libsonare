/// @file mpe_zones_test.cpp
/// @brief The MPE zone model: who owns a channel, what a channel-addressed
///        value means for a note on it, and what the receiver must ignore.
///
/// Two of these cases take their numbers from the specification rather than
/// from this library. The bend combination is Appendix C.6's worked example
/// verbatim -- a manager and a member bend the spec says add to 9 semitones --
/// so reading the wrong sensitivity for either role, or dropping the manager
/// from the sum, fails here rather than agreeing with itself, which no
/// self-consistent round trip could tell us. The prohibition case walks
/// Appendix E Table 5's four rows that name a channel role.
///
/// The first case is the one that guards everything already shipped: before any
/// MCM arrives there are no zones, and an instrument holding one of these must
/// behave exactly as it did without it.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/mpe.h"

namespace {

using sonare::midi::kMpeManagerBendSemitones;
using sonare::midi::kMpeMemberBendSemitones;
using sonare::midi::kMpeUpperManagerChannel;
using sonare::midi::mpe_select_notes;
using sonare::midi::MpeChannelRole;
using sonare::midi::MpeDimension;
using sonare::midi::MpeIgnorable;
using sonare::midi::MpeMidiMode;
using sonare::midi::MpeNote;
using sonare::midi::MpeState;
using sonare::midi::MpeZone;
using sonare::midi::NoteTracking;

/// Lower zone with @p members member channels, which is the configuration
/// Appendix A.2 recommends a one-zone device default to.
MpeState lower_zone(uint8_t members) {
  MpeState state;
  uint16_t moved = 0;
  REQUIRE(state.apply_mcm(0, members, &moved));
  return state;
}

}  // namespace

TEST_CASE("without an MCM no channel is under MPE control", "[midi][mpe]") {
  MpeState state;
  REQUIRE_FALSE(state.active());
  for (uint8_t ch = 0; ch < 16; ++ch) {
    REQUIRE(state.role(ch) == MpeChannelRole::kUnassigned);
    REQUIRE(state.bend_semitones(ch) == 0.0f);
    REQUIRE(state.pressure(ch) == 0);
    REQUIRE(state.timbre(ch) == 0);
    REQUIRE_FALSE(state.has(ch, MpeDimension::kBend));
    // Nothing is refused either: an unassigned channel is ordinary MIDI and the
    // zone model has no opinion about it.
    REQUIRE_FALSE(state.ignores(ch, MpeIgnorable::kPolyKeyPressure));
    REQUIRE_FALSE(state.ignores(ch, MpeIgnorable::kModeMessage));
    REQUIRE_FALSE(state.ignores(ch, MpeIgnorable::kBankSelect));
    REQUIRE_FALSE(state.ignores(ch, MpeIgnorable::kProgramChange));
  }
  // Tracking a value without a zone is harmless and still reads as nothing,
  // which is what keeps a project that sends bend but no MCM unchanged.
  state.track_bend(3, 16383);
  state.track_pressure(3, 100);
  REQUIRE(state.bend_semitones(3) == 0.0f);
  REQUIRE_FALSE(state.has(3, MpeDimension::kPressure));
}

TEST_CASE("an MCM assigns the zone and installs the two bend ranges", "[midi][mpe]") {
  const MpeState state = lower_zone(7);
  REQUIRE(state.active());
  REQUIRE(state.member_count(MpeZone::kLower) == 7);
  REQUIRE(state.member_count(MpeZone::kUpper) == 0);

  REQUIRE(state.role(0) == MpeChannelRole::kManager);
  for (uint8_t ch = 1; ch <= 7; ++ch) {
    REQUIRE(state.role(ch) == MpeChannelRole::kMember);
  }
  // The zone stops where the MCM said it does; the rest of the channel space
  // "remains available for conventional use".
  for (uint8_t ch = 8; ch < 16; ++ch) {
    REQUIRE(state.role(ch) == MpeChannelRole::kUnassigned);
  }

  REQUIRE(state.bend_sensitivity(0) == kMpeManagerBendSemitones);
  REQUIRE(state.bend_sensitivity(1) == kMpeMemberBendSemitones);
  // The two differ by a factor of 24, so a test that read the wrong one would
  // not be within any tolerance of the right answer.
  REQUIRE(kMpeMemberBendSemitones > kMpeManagerBendSemitones);
  REQUIRE(state.midi_mode(MpeZone::kLower) == MpeMidiMode::kPoly);
}

TEST_CASE("an MCM on any other channel changes nothing", "[midi][mpe]") {
  MpeState state = lower_zone(4);
  uint16_t moved = 0xFFFFu;
  for (uint8_t ch = 1; ch < 15; ++ch) {
    REQUIRE_FALSE(state.apply_mcm(ch, 3, &moved));
  }
  // Refused rather than clamped onto the nearest manager: a zone silently
  // rebuilt around a channel the sender did not name reassigns every note.
  REQUIRE(state.member_count(MpeZone::kLower) == 4);
  REQUIRE(state.member_count(MpeZone::kUpper) == 0);
  REQUIRE(moved == 0xFFFFu);  // untouched, so the caller stops nothing
  // A member count past the 15 channels a zone can hold is refused on the same
  // terms.
  REQUIRE_FALSE(state.apply_mcm(0, 16, &moved));
  REQUIRE(state.member_count(MpeZone::kLower) == 4);
}

TEST_CASE("reconfiguring a zone names the channels the caller must silence", "[midi][mpe]") {
  MpeState state = lower_zone(4);  // channels 0..4
  state.track_pressure(2, 90);
  REQUIRE(state.pressure(2) == 90);

  uint16_t moved = 0;
  REQUIRE(state.apply_mcm(0, 2, &moved));  // channels 0..2
  // Channels 3 and 4 left MPE control and are exactly what 2.2.3 asks the
  // receiver to stop and reset.
  REQUIRE(moved == 0b0000000000011000u);
  REQUIRE(state.role(3) == MpeChannelRole::kUnassigned);

  // Channel 2 stayed inside the zone, so it is not in the mask and keeps its
  // tracked value -- a reconfiguration is not a global reset.
  REQUIRE(state.pressure(2) == 90);
  state.reset_controls(moved);
  REQUIRE(state.pressure(2) == 90);
  state.reset_controls(0b0000000000000100u);
  REQUIRE(state.pressure(2) == 0);

  // Re-sending the same configuration moves nothing, so a sender that repeats
  // its MCM does not silence the performance.
  moved = 0xFFFFu;
  REQUIRE(state.apply_mcm(0, 2, &moved));
  REQUIRE(moved == 0);
}

TEST_CASE("the newest MCM takes the overlapping channels", "[midi][mpe]") {
  MpeState state;
  uint16_t moved = 0;
  REQUIRE(state.apply_mcm(kMpeUpperManagerChannel, 7, &moved));  // channels 8..15
  REQUIRE(state.role(8) == MpeChannelRole::kMember);

  // A lower zone reaching into the upper one's channels wins them, and the
  // upper zone keeps only what is left.
  REQUIRE(state.apply_mcm(0, 10, &moved));  // channels 0..10
  REQUIRE(state.member_count(MpeZone::kLower) == 10);
  REQUIRE(state.member_count(MpeZone::kUpper) == 4);  // 11..14, manager 15
  REQUIRE(state.role(10) == MpeChannelRole::kMember);
  REQUIRE(state.zone_of(10) == MpeZone::kLower);
  REQUIRE(state.zone_of(11) == MpeZone::kUpper);

  // Taking every channel leaves the other zone with no members at all, and a
  // zone in that state is deactivated rather than kept as a bare manager.
  REQUIRE(state.apply_mcm(0, 15, &moved));
  REQUIRE(state.member_count(MpeZone::kUpper) == 0);
  REQUIRE(state.role(kMpeUpperManagerChannel) == MpeChannelRole::kMember);
  REQUIRE(state.zone_of(kMpeUpperManagerChannel) == MpeZone::kLower);
}

TEST_CASE("pitch bend sensitivity on one member reaches every member", "[midi][mpe]") {
  MpeState state = lower_zone(4);
  REQUIRE(state.apply_bend_sensitivity(2, 12.0f));
  for (uint8_t ch = 1; ch <= 4; ++ch) {
    REQUIRE(state.bend_sensitivity(ch) == 12.0f);
  }
  // The manager's is its own and did not move with them.
  REQUIRE(state.bend_sensitivity(0) == kMpeManagerBendSemitones);
  REQUIRE(state.apply_bend_sensitivity(0, 5.0f));
  REQUIRE(state.bend_sensitivity(0) == 5.0f);
  REQUIRE(state.bend_sensitivity(1) == 12.0f);

  // Past the range the spec allows an MCM to ask for, and outside the zone.
  REQUIRE(state.apply_bend_sensitivity(1, 500.0f));
  REQUIRE(state.bend_sensitivity(1) == 96.0f);
  REQUIRE_FALSE(state.apply_bend_sensitivity(9, 12.0f));

  // A new MCM reinstalls the defaults, which is what 2.2.5 requires of one.
  uint16_t moved = 0;
  REQUIRE(state.apply_mcm(0, 4, &moved));
  REQUIRE(state.bend_sensitivity(1) == kMpeMemberBendSemitones);
  REQUIRE(state.bend_sensitivity(0) == kMpeManagerBendSemitones);
}

TEST_CASE("manager and member bend combine to the specification's own example", "[midi][mpe]") {
  // Appendix C.6 verbatim: manager sensitivity 2 holding value 16383, member
  // sensitivity 48 holding 9387, which the document says are +2 and +7
  // semitones and total 9. Every number here is the spec's, so reading the
  // wrong sensitivity for either role, or dropping the manager from the sum,
  // lands nowhere near rather than agreeing with itself.
  //
  // What it does NOT separate is 8191 from 8192 as the scale: they differ by
  // 0.0009 semitones here, far inside this tolerance. That divisor is the
  // spec's and is kept for it, not because anything could measure it.
  MpeState state = lower_zone(4);
  state.track_bend(0, 16383);
  state.track_bend(2, 9387);

  REQUIRE(std::fabs(state.manager_bend_semitones(2) - 2.0f) < 0.01f);
  REQUIRE(std::fabs(state.bend_semitones(2) - 9.0f) < 0.01f);
  // A member that bent nothing still carries the manager's bend, and one
  // outside the zone carries neither.
  REQUIRE(std::fabs(state.bend_semitones(3) - 2.0f) < 0.01f);
  REQUIRE(state.bend_semitones(9) == 0.0f);
}

TEST_CASE("an untouched manager is the identity of the combination", "[midi][mpe]") {
  MpeState state = lower_zone(4);
  state.track_pressure(2, 100);
  state.track_timbre(2, 30);
  state.track_bend(2, 9387);

  // Nothing has reached the manager, so the member's values stand unchanged.
  // A manager defaulting to a value instead -- 0 for pressure, the 0x40 centre
  // CC74's relative scheme starts from -- would either be indistinguishable
  // from a real zero or would shift every member value by 64.
  REQUIRE(state.pressure(2) == 100);
  REQUIRE(state.timbre(2) == 30);
  REQUIRE(std::fabs(state.bend_semitones(2) - 7.0f) < 0.01f);

  // Once the manager does send, it biases them, and the sum keeps the domain.
  state.track_pressure(0, 20);
  state.track_timbre(0, 40);
  REQUIRE(state.pressure(2) == 120);
  REQUIRE(state.timbre(2) == 70);
  state.track_pressure(0, 127);
  REQUIRE(state.pressure(2) == 127);

  // And presence is separate from value: a manager that sent an explicit zero
  // has reached the dimension even though it changed nothing.
  MpeState quiet = lower_zone(4);
  REQUIRE_FALSE(quiet.has(2, MpeDimension::kPressure));
  quiet.track_pressure(0, 0);
  REQUIRE(quiet.has(2, MpeDimension::kPressure));
  REQUIRE(quiet.pressure(2) == 0);

  // Bend is where that distinction is invisible in the value: a centred bend
  // and no bend are the same number of semitones, so presence has to be set by
  // the message arriving rather than by the value differing from the default.
  REQUIRE_FALSE(quiet.has(2, MpeDimension::kBend));
  quiet.track_bend(2, 8192);
  REQUIRE(quiet.has(2, MpeDimension::kBend));
  REQUIRE(quiet.bend_semitones(2) == 0.0f);
}

TEST_CASE("a value tracked while the channel is silent is the next note's state", "[midi][mpe]") {
  // 2.2.6 - 2.2.8 each say a receiver "shall continue to track" its dimension
  // even when no note is playing, because the value at Note On is the note's
  // initial state. Nothing here has ever seen a note, which is the point.
  MpeState state = lower_zone(4);
  state.track_timbre(3, 90);
  state.track_pressure(3, 55);
  REQUIRE(state.timbre(3) == 90);
  REQUIRE(state.pressure(3) == 55);
  REQUIRE(state.has(3, MpeDimension::kTimbre));
}

TEST_CASE("the zone model refuses what its channel roles prohibit", "[midi][mpe]") {
  MpeState state = lower_zone(4);

  // Appendix E Table 5, the four rows that turn on a channel's role.
  REQUIRE(state.ignores(2, MpeIgnorable::kPolyKeyPressure));
  REQUIRE_FALSE(state.ignores(0, MpeIgnorable::kPolyKeyPressure));
  REQUIRE(state.ignores(0, MpeIgnorable::kModeMessage));
  REQUIRE_FALSE(state.ignores(2, MpeIgnorable::kModeMessage));
  REQUIRE(state.ignores(2, MpeIgnorable::kBankSelect));
  REQUIRE(state.ignores(2, MpeIgnorable::kProgramChange));

  // Program change and bank select are prohibited on a member channel in Mode 3
  // and permitted in Mode 4, where a controller gives each string its own
  // program. So the two rows move with the mode while the other two do not.
  REQUIRE(state.apply_midi_mode(2, MpeMidiMode::kMono));
  REQUIRE(state.midi_mode(MpeZone::kLower) == MpeMidiMode::kMono);
  REQUIRE_FALSE(state.ignores(2, MpeIgnorable::kBankSelect));
  REQUIRE_FALSE(state.ignores(2, MpeIgnorable::kProgramChange));
  REQUIRE(state.ignores(2, MpeIgnorable::kPolyKeyPressure));
  REQUIRE(state.ignores(0, MpeIgnorable::kModeMessage));

  // The mode message is sent to a member channel, so asking through the manager
  // is refused rather than applied -- the same row that makes ignores() true.
  REQUIRE_FALSE(state.apply_midi_mode(0, MpeMidiMode::kPoly));
  REQUIRE_FALSE(state.apply_midi_mode(9, MpeMidiMode::kPoly));
  REQUIRE(state.midi_mode(MpeZone::kLower) == MpeMidiMode::kMono);
}

TEST_CASE("note tracking picks one note per rule and never a released one", "[midi][mpe]") {
  // Oldest first: 64 started, then 72, then 60.
  auto pick = [](NoteTracking mode) {
    std::array<MpeNote, 3> notes = {MpeNote{64, true}, MpeNote{72, true}, MpeNote{60, true}};
    const size_t count = mpe_select_notes(mode, notes.data(), notes.size());
    std::vector<uint8_t> picked;
    for (const MpeNote& note : notes) {
      if (note.selected) picked.push_back(note.note);
    }
    REQUIRE(picked.size() == count);
    return picked;
  };

  REQUIRE(pick(NoteTracking::kLastNote) == std::vector<uint8_t>{60});
  REQUIRE(pick(NoteTracking::kLowestNote) == std::vector<uint8_t>{60});
  REQUIRE(pick(NoteTracking::kHighestNote) == std::vector<uint8_t>{72});
  REQUIRE(pick(NoteTracking::kAllNotes) == std::vector<uint8_t>{64, 72, 60});
  // The four rules disagree here, which is what says the mode is read at all:
  // last and lowest coincide on this chord only by the order it was played in.
  REQUIRE(pick(NoteTracking::kHighestNote) != pick(NoteTracking::kLastNote));

  // A note whose Note Off has arrived is never selected, by any rule. 2.4 is
  // flat about it -- "the MPE control messages shall not affect a note after
  // the Note Off message has been received" -- and it applies however long a
  // pedal or a release tail keeps the note sounding.
  std::array<MpeNote, 3> released = {MpeNote{64, true}, MpeNote{72, false}, MpeNote{60, false}};
  REQUIRE(mpe_select_notes(NoteTracking::kLastNote, released.data(), released.size()) == 1);
  REQUIRE(released[0].selected);  // the oldest, because it is the only one still held
  REQUIRE(mpe_select_notes(NoteTracking::kAllNotes, released.data(), released.size()) == 1);
  REQUIRE(released[0].selected);
  REQUIRE_FALSE(released[2].selected);

  // Nothing held selects nothing rather than falling back to a released note.
  std::array<MpeNote, 2> all_released = {MpeNote{64, false}, MpeNote{72, false}};
  for (uint8_t mode = 0; mode < sonare::midi::kNoteTrackingCount; ++mode) {
    REQUIRE(mpe_select_notes(static_cast<NoteTracking>(mode), all_released.data(),
                             all_released.size()) == 0);
  }
}
