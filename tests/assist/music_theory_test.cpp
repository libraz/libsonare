/// @file music_theory_test.cpp
/// @brief Pitch-class arithmetic behind the built-in assist modules. Tag:
///        [assist][theory].
///
/// Pins the contracts the modules lean on and that a plausible-looking
/// reimplementation would break: an empty pitch-class set states NO constraint,
/// an extension's quality follows the chord family, and a scale STEP is a
/// position in the set rather than a fixed number of semitones -- the property
/// that makes "a third below" follow the key.

#include "midi/assist/modules/music_theory.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "arrangement/harmonic_timeline.h"

namespace {

using sonare::arrangement::ChordQuality;
using sonare::arrangement::ChordSymbol;
using sonare::arrangement::KeyMode;
using sonare::arrangement::kUnknownPitchClass;
namespace theory = sonare::midi::assist::theory;

std::vector<uint8_t> sorted_unique(std::vector<uint8_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

ChordSymbol chord_of(uint8_t root_pc, ChordQuality quality, std::vector<uint8_t> extensions = {},
                     uint8_t slash_bass_pc = kUnknownPitchClass) {
  ChordSymbol chord;
  chord.start_ppq = 0.0;
  chord.end_ppq = 1920.0;
  chord.root_pc = root_pc;
  chord.quality = quality;
  chord.extensions = std::move(extensions);
  chord.slash_bass_pc = slash_bass_pc;
  return chord;
}

bool contains_pc(const std::vector<uint8_t>& set, uint8_t pc) {
  return std::find(set.begin(), set.end(), pc) != set.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// scale_pitch_classes
// ---------------------------------------------------------------------------

TEST_CASE("scale_pitch_classes spells every mode's diatonic collection on C", "[assist][theory]") {
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kMajor)) ==
        std::vector<uint8_t>{0, 2, 4, 5, 7, 9, 11});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kMinor)) ==
        std::vector<uint8_t>{0, 2, 3, 5, 7, 8, 10});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kDorian)) ==
        std::vector<uint8_t>{0, 2, 3, 5, 7, 9, 10});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kPhrygian)) ==
        std::vector<uint8_t>{0, 1, 3, 5, 7, 8, 10});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kLydian)) ==
        std::vector<uint8_t>{0, 2, 4, 6, 7, 9, 11});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kMixolydian)) ==
        std::vector<uint8_t>{0, 2, 4, 5, 7, 9, 10});
  CHECK(sorted_unique(theory::scale_pitch_classes(0, KeyMode::kLocrian)) ==
        std::vector<uint8_t>{0, 1, 3, 5, 6, 8, 10});
}

TEST_CASE("scale_pitch_classes wraps the collection around a non-C tonic", "[assist][theory]") {
  // G major: the collection wraps past B, and F sharpens.
  CHECK(sorted_unique(theory::scale_pitch_classes(7, KeyMode::kMajor)) ==
        std::vector<uint8_t>{0, 2, 4, 6, 7, 9, 11});
  // A minor is the same collection as C major, one rotation over.
  CHECK(sorted_unique(theory::scale_pitch_classes(9, KeyMode::kMinor)) ==
        sorted_unique(theory::scale_pitch_classes(0, KeyMode::kMajor)));
  // Each mode still has seven distinct pitch classes on every tonic.
  for (uint8_t tonic = 0; tonic < 12; ++tonic) {
    CHECK(sorted_unique(theory::scale_pitch_classes(tonic, KeyMode::kMixolydian)).size() == 7u);
  }
}

TEST_CASE("an unknown mode or an out-of-range tonic states no constraint at all",
          "[assist][theory]") {
  CHECK(theory::scale_pitch_classes(0, KeyMode::kUnknown).empty());
  CHECK(theory::scale_pitch_classes(12, KeyMode::kMajor).empty());
  CHECK(theory::scale_pitch_classes(kUnknownPitchClass, KeyMode::kMajor).empty());

  // Empty means "no constraint stated", never "no pitch class allowed": the
  // difference is what stops an unknown key from rejecting every note.
  const std::vector<uint8_t> unstated = theory::scale_pitch_classes(0, KeyMode::kUnknown);
  CHECK(theory::admits(unstated, 60));
  CHECK(theory::admits(unstated, 61));
  CHECK(theory::admits(unstated, 0));
  CHECK(theory::admits(unstated, 127));
}

// ---------------------------------------------------------------------------
// chord_pitch_classes
// ---------------------------------------------------------------------------

TEST_CASE("chord_pitch_classes spells each family's bare triad", "[assist][theory]") {
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMajor))) ==
        std::vector<uint8_t>{0, 4, 7});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor))) ==
        std::vector<uint8_t>{0, 3, 7});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kDiminished))) ==
        std::vector<uint8_t>{0, 3, 6});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kAugmented))) ==
        std::vector<uint8_t>{0, 4, 8});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kDominant))) ==
        std::vector<uint8_t>{0, 4, 7});
  // Half-diminished carries its seventh in the family, not in the extensions.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kHalfDiminished))) ==
        std::vector<uint8_t>{0, 3, 6, 10});
  // A root other than C moves the whole spelling.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(9, ChordQuality::kMinor))) ==
        std::vector<uint8_t>{0, 4, 9});
}

TEST_CASE("the seventh's size follows the chord family, not the extension number",
          "[assist][theory]") {
  const std::vector<uint8_t> ext7{7};
  // 7 over major is the MAJOR seventh: 11 semitones.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMajor, ext7))) ==
        std::vector<uint8_t>{0, 4, 7, 11});
  // The same 7 over dominant and over minor is the MINOR seventh: 10 semitones.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kDominant, ext7))) ==
        std::vector<uint8_t>{0, 4, 7, 10});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor, ext7))) ==
        std::vector<uint8_t>{0, 3, 7, 10});
  // Diminished takes the diminished seventh, 9 semitones, which no other family
  // spells -- the clearest single case that the degree carries no quality.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kDiminished, ext7))) ==
        std::vector<uint8_t>{0, 3, 6, 9});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kAugmented, ext7))) ==
        std::vector<uint8_t>{0, 4, 8, 10});
}

TEST_CASE("chord_pitch_classes reduces the upper tensions into the octave", "[assist][theory]") {
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kDominant, {7, 9}))) ==
        std::vector<uint8_t>{0, 2, 4, 7, 10});
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(
            0, ChordQuality::kDominant, {7, 9, 13}))) == std::vector<uint8_t>{0, 2, 4, 7, 9, 10});
  // 11 and 4 are the same degree reduced; 13 and 6 likewise.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor, {11}))) ==
        sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor, {4}))));
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor, {13}))) ==
        sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMinor, {6}))));
  // A degree the table does not know is dropped rather than misplaced.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMajor, {5}))) ==
        std::vector<uint8_t>{0, 4, 7});
}

TEST_CASE("a bare suspension is sus4 and a 2 extension makes it sus2", "[assist][theory]") {
  const std::vector<uint8_t> bare =
      theory::chord_pitch_classes(chord_of(0, ChordQuality::kSuspended));
  CHECK(sorted_unique(bare) == std::vector<uint8_t>{0, 5, 7});
  CHECK(contains_pc(bare, 5));
  CHECK_FALSE(contains_pc(bare, 2));
  CHECK_FALSE(contains_pc(bare, 4));  // a suspension has no third

  const std::vector<uint8_t> sus2 =
      theory::chord_pitch_classes(chord_of(0, ChordQuality::kSuspended, {2}));
  CHECK(sorted_unique(sus2) == std::vector<uint8_t>{0, 2, 7});
  CHECK_FALSE(contains_pc(sus2, 5));

  // A sus with a seventh still takes the family's minor seventh.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kSuspended, {7}))) ==
        std::vector<uint8_t>{0, 5, 7, 10});
}

TEST_CASE("a slash bass adds its own pitch class and nothing else", "[assist][theory]") {
  // A over C major is a tone the triad does not have, so the set grows by one.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMajor, {}, 9))) ==
        std::vector<uint8_t>{0, 4, 7, 9});
  // A slash bass already in the chord changes nothing -- the set stays unique.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(0, ChordQuality::kMajor, {}, 4))) ==
        std::vector<uint8_t>{0, 4, 7});
  // An unset slash bass is not a pitch class.
  CHECK(sorted_unique(theory::chord_pitch_classes(chord_of(
            0, ChordQuality::kMajor, {}, kUnknownPitchClass))) == std::vector<uint8_t>{0, 4, 7});
}

TEST_CASE("an unknown quality or an out-of-range root states no chord at all", "[assist][theory]") {
  CHECK(theory::chord_pitch_classes(chord_of(0, ChordQuality::kUnknown)).empty());
  CHECK(theory::chord_pitch_classes(chord_of(kUnknownPitchClass, ChordQuality::kMajor)).empty());
  CHECK(theory::chord_pitch_classes(chord_of(12, ChordQuality::kMajor)).empty());
  CHECK(theory::chord_pitch_classes(ChordSymbol{}).empty());
}

// ---------------------------------------------------------------------------
// interval_class / interval_class_roughness
// ---------------------------------------------------------------------------

TEST_CASE("interval_class is octave-equivalent and inversionally symmetric", "[assist][theory]") {
  CHECK(theory::interval_class(60, 60) == 0);
  CHECK(theory::interval_class(60, 72) == 0);
  CHECK(theory::interval_class(48, 108) == 0);
  // A fifth up and a fourth down are the same interval class.
  CHECK(theory::interval_class(67, 60) == 5);
  CHECK(theory::interval_class(60, 65) == 5);
  CHECK(theory::interval_class(60, 67) == 5);
  // The tritone is its own inversion and is the largest class.
  CHECK(theory::interval_class(60, 66) == 6);
  CHECK(theory::interval_class(66, 60) == 6);
  // Order never matters, at any distance.
  for (int note = 0; note <= 127; ++note) {
    CHECK(theory::interval_class(60, note) == theory::interval_class(note, 60));
    CHECK(theory::interval_class(60, note) >= 0);
    CHECK(theory::interval_class(60, note) <= 6);
  }
}

TEST_CASE("interval_class_roughness ranks the classical consonances", "[assist][theory]") {
  CHECK(theory::interval_class_roughness(0) == Catch::Approx(0.0f));
  CHECK(theory::interval_class_roughness(1) == Catch::Approx(1.0f));
  // The semitone is the worst in-range class, and the tritone sits under it.
  CHECK(theory::interval_class_roughness(6) < theory::interval_class_roughness(1));
  // Fifths, thirds and sixths all sit under the seconds and sevenths.
  CHECK(theory::interval_class_roughness(5) < theory::interval_class_roughness(2));
  CHECK(theory::interval_class_roughness(4) < theory::interval_class_roughness(2));
  CHECK(theory::interval_class_roughness(3) < theory::interval_class_roughness(2));
  for (int ic = 0; ic <= 6; ++ic) {
    CHECK(theory::interval_class_roughness(ic) >= 0.0f);
    CHECK(theory::interval_class_roughness(ic) <= 1.0f);
  }
  // Out of range scores maximally rather than indexing past the table.
  CHECK(theory::interval_class_roughness(-1) == Catch::Approx(1.0f));
  CHECK(theory::interval_class_roughness(7) == Catch::Approx(1.0f));
  CHECK(theory::interval_class_roughness(1000) == Catch::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// admits
// ---------------------------------------------------------------------------

TEST_CASE("admits tests the pitch class and treats an empty set as unconstrained",
          "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);
  CHECK(theory::admits(c_major, 60));
  CHECK(theory::admits(c_major, 72));
  CHECK(theory::admits(c_major, 48));
  CHECK_FALSE(theory::admits(c_major, 61));
  CHECK_FALSE(theory::admits(c_major, 73));
  // An out-of-MIDI note is not admitted by a stated set, but an unstated set
  // still constrains nothing.
  CHECK_FALSE(theory::admits(c_major, -1));
  CHECK_FALSE(theory::admits(c_major, 128));
  CHECK(theory::admits({}, -1));
  CHECK(theory::admits({}, 128));
}

// ---------------------------------------------------------------------------
// transpose_scale_steps -- the property that makes a third follow the key
// ---------------------------------------------------------------------------

TEST_CASE("transpose_scale_steps moves by set positions, so the interval is not constant",
          "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);

  CHECK(theory::transpose_scale_steps(c_major, 60, -2) == 57);
  CHECK(theory::transpose_scale_steps(c_major, 64, -2) == 60);
  CHECK(theory::transpose_scale_steps(c_major, 67, -2) == 64);
  CHECK(theory::transpose_scale_steps(c_major, 72, -2) == 69);

  // The interval a step-move spans DIFFERS note to note. A fixed-semitone
  // implementation passes every same-interval assertion above and fails here.
  const int from_c = 60 - theory::transpose_scale_steps(c_major, 60, -2);
  const int from_e = 64 - theory::transpose_scale_steps(c_major, 64, -2);
  CHECK(from_c == 3);
  CHECK(from_e == 4);
  CHECK(from_c != from_e);
}

TEST_CASE("transpose_scale_steps crosses octaves in both directions", "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);
  // A whole turn of a seven-note set is exactly an octave.
  CHECK(theory::transpose_scale_steps(c_major, 60, -7) == 48);
  CHECK(theory::transpose_scale_steps(c_major, 60, 7) == 72);
  CHECK(theory::transpose_scale_steps(c_major, 60, 14) == 84);
  CHECK(theory::transpose_scale_steps(c_major, 60, -14) == 36);
  // Crossing the boundary partway: B3 up one step is C4, a new octave.
  CHECK(theory::transpose_scale_steps(c_major, 59, 1) == 60);
  // ... and C4 down one step is B3.
  CHECK(theory::transpose_scale_steps(c_major, 60, -1) == 59);
  // Zero steps is identity for a note already in the set.
  CHECK(theory::transpose_scale_steps(c_major, 65, 0) == 65);
}

TEST_CASE("transpose_scale_steps snaps a note outside the set before moving it",
          "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);
  // C sharp is not in C major: it snaps to C first, then moves.
  CHECK(theory::transpose_scale_steps(c_major, 61, -2) == 57);
  CHECK(theory::transpose_scale_steps(c_major, 61, 0) == 60);
  // F sharp snaps to F (the lower of the two equal candidates) and then moves.
  CHECK(theory::transpose_scale_steps(c_major, 66, 0) == 65);
  CHECK(theory::transpose_scale_steps(c_major, 66, 1) == 67);
}

TEST_CASE("transpose_scale_steps has no partial answer to return", "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);
  // An unstated set gives no steps to move through.
  CHECK(theory::transpose_scale_steps({}, 60, -2) == -1);
  CHECK(theory::transpose_scale_steps({}, 60, 0) == -1);
  // A result under 0 or over 127 is refused rather than clamped.
  CHECK(theory::transpose_scale_steps(c_major, 0, -1) == -1);
  CHECK(theory::transpose_scale_steps(c_major, 127, 3) == -1);
  // The notes just inside those boundaries still answer, so the refusals above
  // are the boundary and not a blanket failure.
  CHECK(theory::transpose_scale_steps(c_major, 0, 1) == 2);
  CHECK(theory::transpose_scale_steps(c_major, 127, -1) == 125);
}

TEST_CASE("transpose_scale_steps stays inside its set for every step and start",
          "[assist][theory]") {
  const std::vector<uint8_t> c_minor = theory::scale_pitch_classes(0, KeyMode::kMinor);
  int answered = 0;
  for (int note = 0; note <= 127; ++note) {
    for (int steps = -7; steps <= 7; ++steps) {
      const int moved = theory::transpose_scale_steps(c_minor, note, steps);
      if (moved < 0) continue;
      ++answered;
      CHECK(moved <= 127);
      CHECK(theory::admits(c_minor, moved));
    }
  }
  // The sweep has to have answered, or every assertion in it was skipped.
  CHECK(answered > 1500);
}

// ---------------------------------------------------------------------------
// nearest_admitted
// ---------------------------------------------------------------------------

TEST_CASE("nearest_admitted prefers the lower of two equal distances", "[assist][theory]") {
  const std::vector<uint8_t> c_major = theory::scale_pitch_classes(0, KeyMode::kMajor);
  // C sharp sits one semitone from C below and one from D above: C wins.
  CHECK(theory::nearest_admitted(c_major, 61) == 60);
  CHECK(theory::nearest_admitted(c_major, 63) == 62);
  CHECK(theory::nearest_admitted(c_major, 66) == 65);
  // A note already in the set is returned untouched.
  CHECK(theory::nearest_admitted(c_major, 60) == 60);
  CHECK(theory::nearest_admitted(c_major, 71) == 71);
}

TEST_CASE("nearest_admitted searches outward and answers -1 only when nothing qualifies",
          "[assist][theory]") {
  // A one-note set: the search reaches up to six semitones either way.
  const std::vector<uint8_t> only_c{0};
  CHECK(theory::nearest_admitted(only_c, 65) == 60);
  // The search checks the lower candidate first at each offset, so a note six
  // semitones above a set member resolves downward.
  CHECK(theory::nearest_admitted(only_c, 66) == 60);
  CHECK(theory::nearest_admitted(only_c, 67) == 72);
  // At the top of the MIDI range the upper half of the search is unavailable and
  // the lower half never reaches a C, so there is no answer.
  CHECK(theory::nearest_admitted(only_c, 127) == -1);

  // An unstated set returns the input unchanged -- not -1, and not a snap.
  CHECK(theory::nearest_admitted({}, 61) == 61);
  CHECK(theory::nearest_admitted({}, 0) == 0);
  CHECK(theory::nearest_admitted({}, 127) == 127);
}
