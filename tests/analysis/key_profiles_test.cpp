/// @file key_profiles_test.cpp
/// @brief Tests for key profiles.

#include "analysis/key_profiles.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>

#include "analysis/key_analyzer.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("KS_MAJOR_PROFILE values", "[key_profiles]") {
  // Tonic should be highest
  REQUIRE(KS_MAJOR_PROFILE[0] > KS_MAJOR_PROFILE[1]);

  // Fifth should be second highest
  float fifth = KS_MAJOR_PROFILE[7];
  REQUIRE(fifth > KS_MAJOR_PROFILE[1]);
  REQUIRE(fifth > KS_MAJOR_PROFILE[2]);

  // Profile should have 12 elements
  REQUIRE(KS_MAJOR_PROFILE.size() == 12);
}

TEST_CASE("KS_MINOR_PROFILE values", "[key_profiles]") {
  // Tonic should be highest
  REQUIRE(KS_MINOR_PROFILE[0] > KS_MINOR_PROFILE[1]);

  // Minor third should be relatively high
  float minor_third = KS_MINOR_PROFILE[3];
  REQUIRE(minor_third > KS_MINOR_PROFILE[4]);

  // Profile should have 12 elements
  REQUIRE(KS_MINOR_PROFILE.size() == 12);
}

TEST_CASE("get_major_profile rotation", "[key_profiles]") {
  // C major profile should match KS_MAJOR_PROFILE
  auto c_major = get_major_profile(PitchClass::C);
  for (int i = 0; i < 12; ++i) {
    REQUIRE_THAT(c_major[i], WithinAbs(KS_MAJOR_PROFILE[i], 0.001f));
  }

  // G major should have highest value at G (index 7)
  auto g_major = get_major_profile(PitchClass::G);
  REQUIRE(g_major[7] == g_major[7]);  // Sanity check

  // The tonic (G) should have the highest value
  float max_val = *std::max_element(g_major.begin(), g_major.end());
  REQUIRE_THAT(g_major[7], WithinAbs(max_val, 0.001f));
}

TEST_CASE("get_minor_profile rotation", "[key_profiles]") {
  // A minor profile should have highest value at A (index 9)
  auto a_minor = get_minor_profile(PitchClass::A);

  float max_val = *std::max_element(a_minor.begin(), a_minor.end());
  REQUIRE_THAT(a_minor[9], WithinAbs(max_val, 0.001f));
}

TEST_CASE("get_boosted_major_profile", "[key_profiles]") {
  KeyProfileBoosts boosts;
  boosts.tonic = 1.5f;  // Multiplicative: 1.5x boost
  boosts.fifth = 1.2f;  // Multiplicative: 1.2x boost

  auto c_major = get_major_profile(PitchClass::C);
  auto boosted = get_boosted_major_profile(PitchClass::C, boosts);

  // Tonic should be multiplied by 1.5
  REQUIRE_THAT(boosted[0], WithinAbs(c_major[0] * 1.5f, 0.001f));

  // Fifth should be multiplied by 1.2
  REQUIRE_THAT(boosted[7], WithinAbs(c_major[7] * 1.2f, 0.001f));

  // Other notes should be unchanged (default boost = 1.0)
  REQUIRE_THAT(boosted[1], WithinAbs(c_major[1], 0.001f));
}

TEST_CASE("get_boosted_minor_profile", "[key_profiles]") {
  KeyProfileBoosts boosts;
  boosts.tonic = 1.1f;  // Multiplicative: 1.1x boost
  boosts.third = 1.3f;  // Multiplicative: 1.3x boost (minor third)

  auto a_minor = get_minor_profile(PitchClass::A);
  auto boosted = get_boosted_minor_profile(PitchClass::A, boosts);

  // Tonic (A=9) should be multiplied by 1.1
  REQUIRE_THAT(boosted[9], WithinAbs(a_minor[9] * 1.1f, 0.001f));

  // Minor third (C=0, which is 3 semitones above A) should be multiplied by 1.3
  REQUIRE_THAT(boosted[0], WithinAbs(a_minor[0] * 1.3f, 0.001f));
}

TEST_CASE("normalize_profile", "[key_profiles]") {
  auto profile = KS_MAJOR_PROFILE;
  auto normalized = normalize_profile(profile);

  // Sum should be 1.0
  float sum = std::accumulate(normalized.begin(), normalized.end(), 0.0f);
  REQUIRE_THAT(sum, WithinAbs(1.0f, 0.001f));

  // All values should be non-negative
  for (float val : normalized) {
    REQUIRE(val >= 0.0f);
  }
}

TEST_CASE("profile_correlation identity", "[key_profiles]") {
  // Correlation of a profile with itself should be 1.0
  auto profile = KS_MAJOR_PROFILE;
  float corr = profile_correlation(profile, profile);

  REQUIRE_THAT(corr, WithinAbs(1.0f, 0.001f));
}

TEST_CASE("profile_correlation C major vs A minor", "[key_profiles]") {
  auto c_major = get_major_profile(PitchClass::C);
  auto a_minor = get_minor_profile(PitchClass::A);

  // Create a C major chroma (C, E, G strong)
  std::array<float, 12> c_chroma = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  float corr_major = profile_correlation(c_chroma, c_major);
  float corr_minor = profile_correlation(c_chroma, a_minor);

  // C major chroma should correlate better with C major profile
  REQUIRE(corr_major > corr_minor);
}

TEST_CASE("profile_correlation A minor chroma", "[key_profiles]") {
  auto c_major = get_major_profile(PitchClass::C);
  auto a_minor = get_minor_profile(PitchClass::A);

  // Create an A minor chroma (A, C, E strong)
  std::array<float, 12> a_chroma = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

  float corr_major = profile_correlation(a_chroma, c_major);
  float corr_minor = profile_correlation(a_chroma, a_minor);

  // A minor chroma should correlate better with A minor profile
  REQUIRE(corr_minor > corr_major);
}

TEST_CASE("Temperley profiles differ from KS", "[key_profiles]") {
  auto ks = get_major_profile(PitchClass::C, KeyProfileType::KrumhanslSchmuckler);
  auto temperley = get_major_profile(PitchClass::C, KeyProfileType::Temperley);

  bool different = false;
  for (int i = 0; i < 12; ++i) {
    if (std::abs(ks[i] - temperley[i]) > 0.01f) {
      different = true;
      break;
    }
  }

  REQUIRE(different);
}

TEST_CASE("expanded key profile types expose usable major and minor profiles", "[key_profiles]") {
  const KeyProfileType profile_types[] = {
      KeyProfileType::KrumhanslSchmuckler, KeyProfileType::Temperley,   KeyProfileType::Shaath,
      KeyProfileType::FaraldoEDMT,         KeyProfileType::FaraldoEDMA, KeyProfileType::FaraldoEDMM,
      KeyProfileType::BellmanBudge};

  for (KeyProfileType profile_type : profile_types) {
    CAPTURE(static_cast<int>(profile_type));

    auto major = get_major_profile(PitchClass::C, profile_type);
    auto minor = get_minor_profile(PitchClass::C, profile_type);

    REQUIRE(major.size() == 12);
    REQUIRE(minor.size() == 12);

    for (float value : major) {
      REQUIRE(value >= 0.0f);
    }
    for (float value : minor) {
      REQUIRE(value >= 0.0f);
    }

    auto major_normalized = normalize_profile(major);
    auto minor_normalized = normalize_profile(minor);
    float major_sum = std::accumulate(major_normalized.begin(), major_normalized.end(), 0.0f);
    float minor_sum = std::accumulate(minor_normalized.begin(), minor_normalized.end(), 0.0f);
    REQUIRE_THAT(major_sum, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(minor_sum, WithinAbs(1.0f, 0.001f));
  }
}

TEST_CASE("genre hints resolve without changing auto default behavior", "[key_profiles]") {
  const std::array<float, 12> c_major_profile_chroma =
      normalize_profile(get_boosted_major_profile(PitchClass::C));

  KeyConfig auto_config;
  auto_config.genre_hint = "auto";
  KeyAnalyzer auto_analyzer(c_major_profile_chroma, auto_config);

  KeyConfig explicit_config;
  explicit_config.genre_hint = "";
  explicit_config.profile_type = KeyProfileType::KrumhanslSchmuckler;
  KeyAnalyzer explicit_analyzer(c_major_profile_chroma, explicit_config);

  REQUIRE(auto_analyzer.key().root == explicit_analyzer.key().root);
  REQUIRE(auto_analyzer.key().mode == explicit_analyzer.key().mode);

  KeyConfig edm_config;
  edm_config.genre_hint = "edm";
  KeyAnalyzer edm_analyzer(normalize_profile(get_boosted_minor_profile(
                               PitchClass::A, KeyProfileBoosts(), KeyProfileType::FaraldoEDMA)),
                           edm_config);
  REQUIRE(edm_analyzer.key().root == PitchClass::A);
  REQUIRE(edm_analyzer.key().mode == Mode::Minor);
}

TEST_CASE("auto genre can select a stronger non-KS profile without golden labels",
          "[key_profiles]") {
  const std::array<float, 12> edm_chroma = normalize_profile(
      get_boosted_minor_profile(PitchClass::A, KeyProfileBoosts(), KeyProfileType::FaraldoEDMA));

  KeyConfig ks_config;
  ks_config.genre_hint = "";
  ks_config.profile_type = KeyProfileType::KrumhanslSchmuckler;
  KeyAnalyzer ks_analyzer(edm_chroma, ks_config);

  KeyConfig auto_config;
  auto_config.genre_hint = "auto";
  KeyAnalyzer auto_analyzer(edm_chroma, auto_config);

  REQUIRE(auto_analyzer.key().root == PitchClass::A);
  REQUIRE(auto_analyzer.key().mode == Mode::Minor);
  REQUIRE(auto_analyzer.candidates(1)[0].correlation > ks_analyzer.candidates(1)[0].correlation);
}

TEST_CASE("scale_mask_for_mode matches the scale each mode's key profile weights",
          "[key_profiles]") {
  const struct {
    Mode mode;
    uint16_t expected;
  } kExpected[] = {
      {Mode::Major, 0b101010110101},   {Mode::Minor, 0b010110101101},
      {Mode::Dorian, 0b011010101101},  {Mode::Phrygian, 0b010110101011},
      {Mode::Lydian, 0b101011010101},  {Mode::Mixolydian, 0b011010110101},
      {Mode::Locrian, 0b010101101011},
  };
  for (const auto& row : kExpected) {
    INFO(mode_name(row.mode));
    REQUIRE(scale_mask_for_mode(row.mode) == row.expected);
    uint16_t from_intervals = 0;
    for (int interval : scale_intervals(row.mode)) from_intervals |= uint16_t{1} << interval;
    REQUIRE(from_intervals == row.expected);

    // The five church modes' profiles rank scale degrees above every other degree.
    if (row.mode == Mode::Major || row.mode == Mode::Minor) continue;
    const auto profile = get_mode_profile(PitchClass::C, row.mode);
    const float floor = *std::min_element(profile.begin(), profile.end());
    uint16_t weighted = 0;
    for (int pc = 0; pc < 12; ++pc) {
      if (profile[pc] > floor) weighted |= uint16_t{1} << pc;
    }
    REQUIRE(weighted == row.expected);
  }
}

TEST_CASE("scale_mask_for_mode is the major scale rotated for the relative modes",
          "[key_profiles]") {
  // Every church mode is the major scale started from another degree.
  const uint16_t major = scale_mask_for_mode(Mode::Major);
  const struct {
    Mode mode;
    int degree_semitones;
  } kRotations[] = {{Mode::Dorian, 2},     {Mode::Phrygian, 4}, {Mode::Lydian, 5},
                    {Mode::Mixolydian, 7}, {Mode::Minor, 9},    {Mode::Locrian, 11}};
  for (const auto& row : kRotations) {
    INFO(mode_name(row.mode));
    const unsigned rotated =
        ((major >> row.degree_semitones) | (major << (12 - row.degree_semitones))) & 0x0FFFu;
    REQUIRE(scale_mask_for_mode(row.mode) == rotated);
  }
}
