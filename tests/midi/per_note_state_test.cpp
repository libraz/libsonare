/// @file per_note_state_test.cpp
/// @brief Per-note pitch state: Per-Note Management, pitch priority and composition
///        (M2-104-UM §7.4.5, §7.4.12, §7.4.13, §7.4.15, B.2).

#include "midi/per_note_state.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "midi/control_value.h"

using sonare::midi::apply_per_note_management;
using sonare::midi::Bend32;
using sonare::midi::channel_bend_cents;
using sonare::midi::compose_note_pitch;
using sonare::midi::ComposedPitch;
using sonare::midi::Control32;
using sonare::midi::kDefaultPerNoteBendSensitivity;
using sonare::midi::NotePitchRequest;
using sonare::midi::per_note_bend_units;
using sonare::midi::PerNoteBinding;
using sonare::midi::PerNotePitchTable;

namespace {

/// Q7.25 raw for a whole-and-quarter semitone value (every value used here is exact).
constexpr uint32_t q7_25(double semitones) {
  return static_cast<uint32_t>(semitones * static_cast<double>(uint32_t{1} << 25));
}

/// Minimal stand-in for a synth voice: the module only ever sees the binding.
struct FakeVoice {
  bool active = false;
  PerNoteBinding binding;
  bool has_attr = false;
  uint16_t attr_q7_9 = 0;
};

PerNoteBinding* binding_of(FakeVoice& v) { return v.active ? &v.binding : nullptr; }

void pnm(PerNotePitchTable& table, std::vector<FakeVoice>& voices, uint8_t ch, uint8_t note,
         bool detach, bool reset) {
  apply_per_note_management(table, ch, note, detach, reset, voices.begin(), voices.end(),
                            binding_of);
}

ComposedPitch pitch_of(const FakeVoice& v, const PerNotePitchTable& table, Control32 sensitivity,
                       Bend32 channel_bend, float range_cents) {
  NotePitchRequest req;
  req.note = v.binding.note;
  req.has_attribute_pitch = v.has_attr;
  req.attribute_pitch_q7_9 = v.attr_q7_9;
  req.per_note = v.binding.pitch_inputs(table);
  req.per_note_bend_sensitivity = sensitivity;
  req.channel_bend = channel_bend;
  req.channel_bend_range_cents = range_cents;
  return compose_note_pitch(req);
}

bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

// Pairwise rows over seven parameters (coverwise, strength 2, 136/136 pairs, 17 rows).
// Fixed scenario: channel 3, note 60; attribute 7.9 = 64.5; RPNC #3 = 62.25; per-note bend
// up = 0xFFFFFFFF (+1 x sensitivity), down = 0x40000000 (-0.5 x), center = 0x80000000;
// sensitivity default 2 / custom 12 semitones; channel bend up = 14-bit 12288 at a 200-cent
// range (+1 semitone).
// before_note: row values -> PNM -> note-on. during_note: row values -> note-on -> PNM ->
// per-note bend 0x60000000 (-0.25 x sensitivity). The expected pitch is worked out by hand.
TEST_CASE("per_note pitch rules: pairwise table", "[midi][midi2]") {
  enum class Pnb { kUnset, kCenter, kUp, kDown };
  enum class Pnm { kNone, kD, kS, kDS };
  struct Row {
    bool attr79;
    bool rpnc3;
    Pnb bend;
    Pnm pnm;
    bool during_note;
    bool custom_sensitivity;
    bool channel_bend_up;
    double expected;
    bool absolute;
  };
  constexpr bool B = false;  // before_note
  constexpr bool D = true;   // during_note
  const std::array<Row, 17> rows = {{
      {true, false, Pnb::kCenter, Pnm::kS, B, false, true, 65.5, true},
      {true, true, Pnb::kDown, Pnm::kNone, D, true, false, 61.5, true},
      {false, false, Pnb::kUp, Pnm::kNone, B, true, false, 72.0, false},
      {false, true, Pnb::kUnset, Pnm::kD, D, false, true, 63.25, true},
      {true, false, Pnb::kUnset, Pnm::kDS, B, false, false, 64.5, true},
      {false, false, Pnb::kDown, Pnm::kDS, D, true, true, 55.0, false},
      {false, true, Pnb::kUp, Pnm::kS, B, false, true, 61.0, false},
      {false, true, Pnb::kCenter, Pnm::kD, B, true, false, 62.25, true},
      {true, true, Pnb::kUnset, Pnm::kS, B, true, true, 65.5, true},
      {true, false, Pnb::kDown, Pnm::kD, B, false, false, 63.5, true},
      {true, true, Pnb::kCenter, Pnm::kNone, D, false, true, 65.0, true},
      {false, true, Pnb::kCenter, Pnm::kDS, B, false, true, 61.0, false},
      {true, true, Pnb::kUp, Pnm::kD, B, true, true, 77.5, true},
      {false, true, Pnb::kDown, Pnm::kS, D, false, false, 59.5, false},
      {true, false, Pnb::kUnset, Pnm::kNone, B, true, false, 64.5, true},
      {false, true, Pnb::kUp, Pnm::kNone, D, false, false, 61.75, true},
      {false, true, Pnb::kUp, Pnm::kDS, B, true, false, 60.0, false},
  }};

  constexpr uint8_t kCh = 3;
  constexpr uint8_t kNote = 60;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    INFO("row " << i);
    PerNotePitchTable table;
    std::vector<FakeVoice> voices(4);
    if (r.rpnc3) table.set_pitch_7_25(kCh, kNote, Control32::from_raw(q7_25(62.25)));
    switch (r.bend) {
      case Pnb::kUnset:
        break;
      case Pnb::kCenter:
        table.set_per_note_bend(kCh, kNote, Bend32::center());
        break;
      case Pnb::kUp:
        table.set_per_note_bend(kCh, kNote, Bend32::from_raw(0xFFFFFFFFu));
        break;
      case Pnb::kDown:
        table.set_per_note_bend(kCh, kNote, Bend32::from_raw(0x40000000u));
        break;
    }
    const bool detach = r.pnm == Pnm::kD || r.pnm == Pnm::kDS;
    const bool reset = r.pnm == Pnm::kS || r.pnm == Pnm::kDS;
    const bool any_pnm = r.pnm != Pnm::kNone;
    if (!r.during_note && any_pnm) pnm(table, voices, kCh, kNote, detach, reset);
    FakeVoice& v = voices[1];
    v.active = true;
    v.binding.bind(kCh, kNote);
    v.has_attr = r.attr79;
    v.attr_q7_9 = static_cast<uint16_t>((64u << 9) | 256u);  // 64.5
    if (r.during_note) {
      if (any_pnm) pnm(table, voices, kCh, kNote, detach, reset);
      table.set_per_note_bend(kCh, kNote, Bend32::from_raw(0x60000000u));
    }
    const Control32 sens =
        r.custom_sensitivity ? Control32::from_raw(q7_25(12.0)) : kDefaultPerNoteBendSensitivity;
    const Bend32 cb = r.channel_bend_up ? Bend32::from14(12288) : Bend32::from14(8192);
    const ComposedPitch p = pitch_of(v, table, sens, cb, 200.0f);
    CHECK(p.pitch_semitones == r.expected);
    CHECK(p.absolute == r.absolute);
  }
}

TEST_CASE("per_note PNM detach freezes sounding voices, a new note binds to the row",
          "[midi][midi2]") {
  PerNotePitchTable table;
  std::vector<FakeVoice> voices(3);
  table.set_per_note_bend(0, 60, Bend32::from_raw(0xFFFFFFFFu));  // +2 semitones
  voices[0].active = true;
  voices[0].binding.bind(0, 60);
  voices[2].active = true;
  voices[2].binding.bind(0, 61);  // other key: untouched by a PNM on 60

  pnm(table, voices, 0, 60, true, false);
  CHECK(voices[0].binding.detached);
  CHECK_FALSE(voices[2].binding.detached);

  // Per-note values persist on the key: a new note on the same key picks up the row.
  voices[1].active = true;
  voices[1].binding.bind(0, 60);
  CHECK_FALSE(voices[1].binding.detached);
  const auto sens = kDefaultPerNoteBendSensitivity;
  CHECK(pitch_of(voices[1], table, sens, Bend32::center(), 200.0f).pitch_semitones == 62.0);

  table.set_per_note_bend(0, 60, Bend32::from_raw(0x00000000u));  // -2 semitones
  CHECK(pitch_of(voices[0], table, sens, Bend32::center(), 200.0f).pitch_semitones == 62.0);
  CHECK(pitch_of(voices[1], table, sens, Bend32::center(), 200.0f).pitch_semitones == 58.0);
  CHECK(pitch_of(voices[2], table, sens, Bend32::center(), 200.0f).pitch_semitones == 61.0);
}

TEST_CASE("per_note PNM reset clears the row, detach-and-reset keeps the voice's values",
          "[midi][midi2]") {
  const auto sens = kDefaultPerNoteBendSensitivity;
  SECTION("S only: the attached voice follows the cleared row") {
    PerNotePitchTable table;
    std::vector<FakeVoice> voices(1);
    table.set_pitch_7_25(5, 70, Control32::from_raw(q7_25(71.5)));
    voices[0].active = true;
    voices[0].binding.bind(5, 70);
    CHECK(pitch_of(voices[0], table, sens, Bend32::center(), 200.0f).pitch_semitones == 71.5);
    pnm(table, voices, 5, 70, false, true);
    CHECK_FALSE(voices[0].binding.detached);
    CHECK_FALSE(table.row(5, 70).pitch_7_25_set);
    CHECK_FALSE(table.row(5, 70).bend_set);
    const ComposedPitch p = pitch_of(voices[0], table, sens, Bend32::center(), 200.0f);
    CHECK(p.pitch_semitones == 70.0);
    CHECK_FALSE(p.absolute);
  }
  SECTION("D+S: detach first, so the voice keeps the pre-reset values") {
    PerNotePitchTable table;
    std::vector<FakeVoice> voices(1);
    table.set_pitch_7_25(5, 70, Control32::from_raw(q7_25(71.5)));
    voices[0].active = true;
    voices[0].binding.bind(5, 70);
    pnm(table, voices, 5, 70, true, true);
    CHECK(voices[0].binding.detached);
    CHECK_FALSE(table.row(5, 70).pitch_7_25_set);
    CHECK(pitch_of(voices[0], table, sens, Bend32::center(), 200.0f).pitch_semitones == 71.5);
  }
  SECTION("D=0 S=0 has no function") {
    PerNotePitchTable table;
    std::vector<FakeVoice> voices(1);
    table.set_pitch_7_25(5, 70, Control32::from_raw(q7_25(71.5)));
    voices[0].active = true;
    voices[0].binding.bind(5, 70);
    pnm(table, voices, 5, 70, false, false);
    CHECK_FALSE(voices[0].binding.detached);
    CHECK(table.row(5, 70).pitch_7_25_set);
  }
  SECTION("idle voices are skipped") {
    PerNotePitchTable table;
    std::vector<FakeVoice> voices(1);
    voices[0].binding.bind(5, 70);  // left over from an earlier note; voice inactive
    pnm(table, voices, 5, 70, true, false);
    CHECK_FALSE(voices[0].binding.detached);
  }
}

TEST_CASE("per_note pitch priority: 7.9 over 7.25 over note number", "[midi][midi2]") {
  NotePitchRequest req;
  req.note = 60;
  ComposedPitch p = compose_note_pitch(req);
  CHECK(p.pitch_semitones == 60.0);
  CHECK_FALSE(p.absolute);

  req.per_note.pitch_7_25 = Control32::from_raw(q7_25(61.75));
  req.per_note.pitch_7_25_set = true;
  p = compose_note_pitch(req);
  CHECK(p.pitch_semitones == 61.75);
  CHECK(p.absolute);

  req.has_attribute_pitch = true;
  req.attribute_pitch_q7_9 = static_cast<uint16_t>((59u << 9) | 128u);  // 59.25
  p = compose_note_pitch(req);
  CHECK(p.pitch_semitones == 59.25);
  CHECK(p.absolute);
  CHECK(p.per_note_semitones == -0.75);

  // 7.9 on its own, with nothing in the row.
  req.per_note.pitch_7_25_set = false;
  p = compose_note_pitch(req);
  CHECK(p.pitch_semitones == 59.25);
  CHECK(p.absolute);
}

TEST_CASE("per_note pitch: every relative term adds to the base", "[midi][midi2]") {
  NotePitchRequest req;
  req.note = 48;
  req.per_note.pitch_7_25 = Control32::from_raw(q7_25(50.0));
  req.per_note.pitch_7_25_set = true;
  req.per_note.bend = Bend32::from_raw(0x00000000u);  // -1 x sensitivity
  req.per_note.bend_set = true;
  req.per_note_bend_sensitivity = Control32::from_raw(q7_25(0.5));
  req.channel_bend = Bend32::from14(4096);  // -0.5 x range
  req.channel_bend_range_cents = 1200.0f;   // -6 semitones
  req.coarse_tune_semitones = 3.0;
  req.fine_tune_semitones = 0.25;
  req.mpe_bend_semitones = -1.5;
  // 50 - 0.5 - 6 + 3 + 0.25 - 1.5
  const ComposedPitch p = compose_note_pitch(req);
  CHECK(p.pitch_semitones == 45.25);
  CHECK(p.absolute);
  CHECK(p.per_note_semitones == 1.5);  // (50 - 48) - 0.5
}

TEST_CASE("per_note bend sensitivity: default 2 semitones, RC 0/7 as Q7.25", "[midi][midi2]") {
  CHECK(kDefaultPerNoteBendSensitivity.q7_25() == 2.0);

  NotePitchRequest req;
  req.note = 60;
  req.per_note.bend = Bend32::from_raw(0xFFFFFFFFu);
  req.per_note.bend_set = true;
  CHECK(compose_note_pitch(req).pitch_semitones == 62.0);
  req.per_note_bend_sensitivity = Control32::from_raw(q7_25(48.0));
  CHECK(compose_note_pitch(req).pitch_semitones == 108.0);
  req.per_note_bend_sensitivity = Control32::from_raw(q7_25(0.75));
  CHECK(compose_note_pitch(req).pitch_semitones == 60.75);

  // Unset bend contributes nothing whatever the sensitivity.
  req.per_note.bend_set = false;
  CHECK(compose_note_pitch(req).pitch_semitones == 60.0);
}

TEST_CASE("per_note bend mapping: centered bipolar, equal range each side", "[midi][midi2]") {
  CHECK(per_note_bend_units(Bend32::from_raw(0x00000000u)) == -1.0);
  CHECK(per_note_bend_units(Bend32::from_raw(0x40000000u)) == -0.5);
  CHECK(per_note_bend_units(Bend32::center()) == 0.0);
  CHECK(per_note_bend_units(Bend32::from_raw(0xFFFFFFFFu)) == 1.0);
  CHECK(per_note_bend_units(Bend32::from_raw(0x80000001u)) > 0.0);
  CHECK(per_note_bend_units(Bend32::from_raw(0x7FFFFFFFu)) < 0.0);
}

TEST_CASE("per_note state is not touched by channel controller resets", "[midi][midi2]") {
  // Reset All Controllers returns the channel bend to center and leaves the per-note row in
  // place (M2-104-UM B.2); the table carries no channel-wide reset for a RAC handler to call.
  PerNotePitchTable table;
  std::vector<FakeVoice> voices(1);
  table.set_per_note_bend(2, 64, Bend32::from_raw(0x40000000u));  // -1 semitone at default
  table.set_pitch_7_25(2, 64, Control32::from_raw(q7_25(64.5)));
  voices[0].active = true;
  voices[0].binding.bind(2, 64);
  const auto sens = kDefaultPerNoteBendSensitivity;
  CHECK(pitch_of(voices[0], table, sens, Bend32::from14(12288), 200.0f).pitch_semitones == 64.5);
  // What a RAC does to the inputs this module sees: channel bend back to center.
  CHECK(pitch_of(voices[0], table, sens, Bend32::center(), 200.0f).pitch_semitones == 63.5);
  CHECK(table.row(2, 64).bend_set);
  CHECK(table.row(2, 64).pitch_7_25_set);
}

TEST_CASE("per_note state is isolated per channel and per note", "[midi][midi2]") {
  PerNotePitchTable table;
  table.set_per_note_bend(0, 60, Bend32::from_raw(0xFFFFFFFFu));
  table.set_pitch_7_25(0, 60, Control32::from_raw(q7_25(61.0)));
  CHECK_FALSE(table.row(1, 60).bend_set);
  CHECK_FALSE(table.row(1, 60).pitch_7_25_set);
  CHECK_FALSE(table.row(0, 61).bend_set);
  CHECK_FALSE(table.row(15, 60).pitch_7_25_set);

  std::vector<FakeVoice> voices(2);
  voices[0].active = true;
  voices[0].binding.bind(0, 60);
  voices[1].active = true;
  voices[1].binding.bind(1, 60);
  pnm(table, voices, 1, 60, true, true);
  CHECK_FALSE(voices[0].binding.detached);
  CHECK(voices[1].binding.detached);
  CHECK(table.row(0, 60).bend_set);
  CHECK(table.row(0, 60).pitch_7_25_set);

  table.clear();
  CHECK_FALSE(table.row(0, 60).bend_set);
  CHECK_FALSE(table.row(0, 60).pitch_7_25_set);
}

TEST_CASE("per_note pitch: MIDI 1.0 input reproduces the synth's channel bend exactly",
          "[midi][midi2]") {
  // NativeSynth and Sf2Player compute (float(pb) - 8192) / 8192 * bend_range_cents in float.
  const std::array<float, 4> ranges = {200.0f, 1200.0f, 250.0f, 0.0f};
  size_t mismatches = 0;
  size_t per_note_nonzero = 0;
  for (float range : ranges) {
    for (uint32_t v = 0; v < 16384u; ++v) {
      const float synth = (static_cast<float>(v) - 8192.0f) / 8192.0f * range;
      const Bend32 b = Bend32::from14(static_cast<uint16_t>(v));
      if (!same_bits(channel_bend_cents(b, range), synth)) ++mismatches;
      NotePitchRequest req;
      req.note = 69;
      req.channel_bend = b;
      req.channel_bend_range_cents = range;
      const ComposedPitch p = compose_note_pitch(req);
      if (p.per_note_semitones != 0.0 || p.absolute ||
          p.pitch_semitones != 69.0 + static_cast<double>(synth) / 100.0) {
        ++per_note_nonzero;
      }
    }
  }
  CHECK(mismatches == 0);
  CHECK(per_note_nonzero == 0);
}
