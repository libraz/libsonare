/// @file key_profiles_test.cpp
/// @brief Tests for key profiles.

#include "analysis/key_profiles.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>

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
  boosts.tonic = 1.5f;   // Multiplicative: 1.5x boost
  boosts.fifth = 1.2f;   // Multiplicative: 1.2x boost

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
  boosts.tonic = 1.1f;   // Multiplicative: 1.1x boost
  boosts.third = 1.3f;   // Multiplicative: 1.3x boost (minor third)

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
