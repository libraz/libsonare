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
