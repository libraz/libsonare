/// @file key_synthetic_matrix_test.cpp
/// @brief Synthetic matrix tests for key detection.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/key_profiles.h"

using namespace sonare;

namespace {

std::array<float, 12> boosted_major_chroma(PitchClass root, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMajorTonicBoost;
  boosts.third = key_constants::kMajorThirdBoost;
  boosts.fifth = key_constants::kMajorFifthBoost;
  boosts.seventh = 1.0f;
  return normalize_profile(get_boosted_major_profile(root, boosts, profile_type));
}

std::array<float, 12> boosted_minor_chroma(PitchClass root, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMinorTonicBoost;
  boosts.third = key_constants::kMinorThirdBoost;
  boosts.fifth = key_constants::kMinorFifthBoost;
  boosts.seventh = key_constants::kMinorSeventhBoost;
  return normalize_profile(get_boosted_minor_profile(root, boosts, profile_type));
}

std::array<float, 12> boosted_mode_chroma(PitchClass root, Mode mode, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMinorTonicBoost;
  boosts.third = key_constants::kMinorThirdBoost;
  boosts.fifth = key_constants::kMinorFifthBoost;
  boosts.seventh = key_constants::kMinorSeventhBoost;
  return normalize_profile(get_boosted_mode_profile(root, mode, boosts, profile_type));
}

Chord chord(PitchClass root, ChordQuality quality, float start, float end) {
  return Chord{root, quality, start, end, 1.0f};
}

PitchClass transpose(PitchClass root, int semitones) {
  return static_cast<PitchClass>((static_cast<int>(root) + semitones) % 12);
}

}  // namespace

TEST_CASE("KeyAnalyzer synthetic chroma matrix detects all major and minor keys",
          "[key_analyzer][synthetic_matrix]") {
  for (KeyProfileType profile_type :
       {KeyProfileType::KrumhanslSchmuckler, KeyProfileType::Temperley, KeyProfileType::Shaath,
        KeyProfileType::FaraldoEDMT, KeyProfileType::FaraldoEDMA, KeyProfileType::FaraldoEDMM,
        KeyProfileType::BellmanBudge}) {
    KeyConfig config;
    config.profile_type = profile_type;

    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      auto root = static_cast<PitchClass>(root_idx);

      CAPTURE(root_idx);
      CAPTURE(static_cast<int>(profile_type));

      KeyAnalyzer major_analyzer(boosted_major_chroma(root, profile_type), config);
      REQUIRE(major_analyzer.key().root == root);
      REQUIRE(major_analyzer.key().mode == Mode::Major);
      REQUIRE(major_analyzer.key().confidence > 0.5f);

      KeyAnalyzer minor_analyzer(boosted_minor_chroma(root, profile_type), config);
      REQUIRE(minor_analyzer.key().root == root);
      REQUIRE(minor_analyzer.key().mode == Mode::Minor);
      REQUIRE(minor_analyzer.key().confidence > 0.5f);
    }
  }
}

TEST_CASE("estimate_key_from_chords synthetic cadence matrix resolves all tonics",
          "[key_analyzer][synthetic_matrix]") {
  for (int root_idx = 0; root_idx < 12; ++root_idx) {
    auto tonic = static_cast<PitchClass>(root_idx);

    CAPTURE(root_idx);

    std::vector<Chord> major_cadence = {
        chord(tonic, ChordQuality::Major, 0.0f, 1.0f),
        chord(transpose(tonic, 5), ChordQuality::Major, 1.0f, 2.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 2.0f, 3.0f),
        chord(tonic, ChordQuality::Major, 3.0f, 4.0f),
    };
    Key major_key = estimate_key_from_chords(major_cadence);
    REQUIRE(major_key.root == tonic);
    REQUIRE(major_key.mode == Mode::Major);
    REQUIRE(major_key.confidence > 0.5f);

    std::vector<Chord> minor_cadence = {
        chord(tonic, ChordQuality::Minor, 0.0f, 1.0f),
        chord(transpose(tonic, 5), ChordQuality::Minor, 1.0f, 2.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 2.0f, 3.0f),
        chord(tonic, ChordQuality::Minor, 3.0f, 4.0f),
    };
    Key minor_key = estimate_key_from_chords(minor_cadence);
    REQUIRE(minor_key.root == tonic);
    REQUIRE(minor_key.mode == Mode::Minor);
    REQUIRE(minor_key.confidence > 0.5f);
  }
}

TEST_CASE("estimate_key_from_chords scores minor cadence/bookend bonuses symmetrically",
          "[key_analyzer][synthetic_matrix]") {
  // Regression for the major-bias defect: the minor branch used to receive no
  // cadence/bookend/first-chord bonus, so a progression that ends on an authentic
  // minor cadence (V major -> i minor) but contains prominent relative-major
  // chords was mis-detected as the relative major. Here C/G/C give C major a
  // strong tonic+dominant+first-chord lead, but the piece resolves E -> Am (an
  // A-minor V-i). With the symmetric minor cadence bonus the correct A-minor
  // reading wins; without it the relative major (C) would.
  std::vector<Chord> minor_cadence_in_major_context = {
      chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f),
      chord(PitchClass::G, ChordQuality::Major, 1.0f, 2.0f),
      chord(PitchClass::C, ChordQuality::Major, 2.0f, 3.0f),
      chord(PitchClass::E, ChordQuality::Major, 3.0f, 4.0f),
      chord(PitchClass::A, ChordQuality::Minor, 4.0f, 5.0f),
  };
  Key key = estimate_key_from_chords(minor_cadence_in_major_context);
  REQUIRE(key.root == PitchClass::A);
  REQUIRE(key.mode == Mode::Minor);

  // The analogous all-minor bookended vamp (i ... i with a minor tonic at both
  // ends) must also resolve to minor rather than its relative major.
  std::vector<Chord> minor_bookended = {
      chord(PitchClass::A, ChordQuality::Minor, 0.0f, 1.0f),
      chord(PitchClass::D, ChordQuality::Minor, 1.0f, 2.0f),
      chord(PitchClass::E, ChordQuality::Major, 2.0f, 3.0f),
      chord(PitchClass::A, ChordQuality::Minor, 3.0f, 4.0f),
  };
  Key bookend_key = estimate_key_from_chords(minor_bookended);
  REQUIRE(bookend_key.root == PitchClass::A);
  REQUIRE(bookend_key.mode == Mode::Minor);
}

TEST_CASE("estimate_key_from_chords does not bias zero-score progressions to C major",
          "[key_analyzer][synthetic_matrix]") {
  std::vector<Chord> zero_duration = {
      chord(PitchClass::Fs, ChordQuality::Minor, 0.0f, 0.0f),
      chord(PitchClass::B, ChordQuality::Minor, 0.0f, 0.0f),
  };
  Key key = estimate_key_from_chords(zero_duration);
  REQUIRE(key.root == PitchClass::Fs);
  REQUIRE(key.mode == Mode::Minor);
  REQUIRE(key.confidence == 0.0f);
}

TEST_CASE("Modal profiles identify a mode from a scale histogram they were not built from",
          "[key_analyzer][key_profiles][synthetic_matrix]") {
  // The matrix case below feeds each profile back to itself, which proves the
  // profiles are distinguishable but says nothing about whether they describe
  // the mode they claim to. This one builds the chroma from the *scale* instead:
  // flat weight on every scale degree, a tonic and fifth emphasis that says
  // which note the scale is heard from, and nothing outside the scale. The
  // weights are chosen here and share no value with the profile construction,
  // so a profile that merely encodes its own shape cannot pass.
  //
  // The emphasis is what the case turns on. Every mode shares its pitch-class
  // set with six others, so a histogram with no tonic emphasis identifies the
  // set and not the mode; the profiles have to read the emphasis.
  struct ModeScale {
    Mode mode;
    std::array<int, 7> degrees;
  };
  const ModeScale scales[] = {
      {Mode::Dorian, {0, 2, 3, 5, 7, 9, 10}},  {Mode::Phrygian, {0, 1, 3, 5, 7, 8, 10}},
      {Mode::Lydian, {0, 2, 4, 6, 7, 9, 11}},  {Mode::Mixolydian, {0, 2, 4, 5, 7, 9, 10}},
      {Mode::Locrian, {0, 1, 3, 5, 6, 8, 10}},
  };

  KeyConfig config;
  config.modes = {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
                  Mode::Lydian, Mode::Mixolydian, Mode::Locrian};

  for (const ModeScale& entry : scales) {
    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      const auto root = static_cast<PitchClass>(root_idx);
      CAPTURE(root_idx, static_cast<int>(entry.mode));

      std::array<float, 12> chroma{};
      chroma.fill(0.05f);
      for (int degree : entry.degrees) {
        chroma[(root_idx + degree) % 12] = 1.0f;
      }
      chroma[root_idx] = 2.0f;
      // Locrian is the one mode whose fifth is diminished; emphasising the
      // perfect fifth there would describe a scale it does not have.
      const int fifth = entry.mode == Mode::Locrian ? 6 : 7;
      chroma[(root_idx + fifth) % 12] = 1.4f;

      KeyAnalyzer analyzer(chroma, config);
      CAPTURE(analyzer.key().to_string());
      REQUIRE(analyzer.key().root == root);
      REQUIRE(analyzer.key().mode == entry.mode);
    }
  }
}

TEST_CASE("KeyAnalyzer modal synthetic matrix is opt-in", "[key_analyzer][synthetic_matrix]") {
  const Mode modes[] = {Mode::Dorian, Mode::Phrygian, Mode::Lydian, Mode::Mixolydian,
                        Mode::Locrian};

  KeyConfig config;
  config.modes = {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
                  Mode::Lydian, Mode::Mixolydian, Mode::Locrian};

  for (Mode mode : modes) {
    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      auto root = static_cast<PitchClass>(root_idx);

      CAPTURE(root_idx);
      CAPTURE(static_cast<int>(mode));

      KeyAnalyzer analyzer(boosted_mode_chroma(root, mode, config.profile_type), config);
      REQUIRE(analyzer.key().root == root);
      REQUIRE(analyzer.key().mode == mode);
      REQUIRE(analyzer.key().confidence > 0.5f);
    }
  }
}
