/// @file chord_templates_test.cpp
/// @brief Tests for chord templates.

#include "analysis/chord_templates.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("create_major_template", "[chord_templates]") {
  auto c_major = create_major_template(PitchClass::C);

  REQUIRE(c_major.root == PitchClass::C);
  REQUIRE(c_major.quality == ChordQuality::Major);

  // C major = C, E, G (indices 0, 4, 7)
  REQUIRE_THAT(c_major.pattern[0], WithinAbs(1.0f, 0.001f));  // C
  REQUIRE_THAT(c_major.pattern[4], WithinAbs(1.0f, 0.001f));  // E
  REQUIRE_THAT(c_major.pattern[7], WithinAbs(1.0f, 0.001f));  // G

  // Other notes should be 0
  REQUIRE_THAT(c_major.pattern[1], WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(c_major.pattern[2], WithinAbs(0.0f, 0.001f));
}

TEST_CASE("create_minor_template", "[chord_templates]") {
  auto a_minor = create_minor_template(PitchClass::A);

  REQUIRE(a_minor.root == PitchClass::A);
  REQUIRE(a_minor.quality == ChordQuality::Minor);

  // A minor = A, C, E (indices 9, 0, 4)
  REQUIRE_THAT(a_minor.pattern[9], WithinAbs(1.0f, 0.001f));  // A
  REQUIRE_THAT(a_minor.pattern[0], WithinAbs(1.0f, 0.001f));  // C
  REQUIRE_THAT(a_minor.pattern[4], WithinAbs(1.0f, 0.001f));  // E
}

TEST_CASE("create_diminished_template", "[chord_templates]") {
  auto b_dim = create_diminished_template(PitchClass::B);

  REQUIRE(b_dim.quality == ChordQuality::Diminished);

  // B diminished = B, D, F (indices 11, 2, 5)
  REQUIRE_THAT(b_dim.pattern[11], WithinAbs(1.0f, 0.001f));  // B
  REQUIRE_THAT(b_dim.pattern[2], WithinAbs(1.0f, 0.001f));   // D
  REQUIRE_THAT(b_dim.pattern[5], WithinAbs(1.0f, 0.001f));   // F
}

TEST_CASE("create_dominant7_template", "[chord_templates]") {
  auto g7 = create_dominant7_template(PitchClass::G);

  REQUIRE(g7.quality == ChordQuality::Dominant7);

  // G7 = G, B, D, F (indices 7, 11, 2, 5)
  REQUIRE_THAT(g7.pattern[7], WithinAbs(1.0f, 0.001f));   // G
  REQUIRE_THAT(g7.pattern[11], WithinAbs(1.0f, 0.001f));  // B
  REQUIRE_THAT(g7.pattern[2], WithinAbs(1.0f, 0.001f));   // D
  REQUIRE_THAT(g7.pattern[5], WithinAbs(1.0f, 0.001f));   // F
}

TEST_CASE("ChordTemplate to_string", "[chord_templates]") {
  auto c_major = create_major_template(PitchClass::C);
  REQUIRE(c_major.to_string() == "Cmaj");

  auto a_minor = create_minor_template(PitchClass::A);
  REQUIRE(a_minor.to_string() == "Am");

  auto g7 = create_dominant7_template(PitchClass::G);
  REQUIRE(g7.to_string() == "G7");

  auto fs_dim = create_diminished_template(PitchClass::Fs);
  REQUIRE(fs_dim.to_string() == "F#dim");
}

TEST_CASE("transpose_template", "[chord_templates]") {
  auto c_major = create_major_template(PitchClass::C);
  auto g_major = transpose_template(c_major, 7);

  REQUIRE(g_major.root == PitchClass::G);
  REQUIRE(g_major.quality == ChordQuality::Major);

  // G major = G, B, D (indices 7, 11, 2)
  REQUIRE_THAT(g_major.pattern[7], WithinAbs(1.0f, 0.001f));   // G
  REQUIRE_THAT(g_major.pattern[11], WithinAbs(1.0f, 0.001f));  // B
  REQUIRE_THAT(g_major.pattern[2], WithinAbs(1.0f, 0.001f));   // D
}

TEST_CASE("transpose_template negative", "[chord_templates]") {
  auto g_major = create_major_template(PitchClass::G);
  auto c_major = transpose_template(g_major, -7);

  REQUIRE(c_major.root == PitchClass::C);

  // C major = C, E, G (indices 0, 4, 7)
  REQUIRE_THAT(c_major.pattern[0], WithinAbs(1.0f, 0.001f));
  REQUIRE_THAT(c_major.pattern[4], WithinAbs(1.0f, 0.001f));
  REQUIRE_THAT(c_major.pattern[7], WithinAbs(1.0f, 0.001f));
}

TEST_CASE("generate_all_chord_templates", "[chord_templates]") {
  auto templates = generate_all_chord_templates();

  // 12 roots × 9 qualities = 108 templates
  REQUIRE(templates.size() == 108);

  // Check that all roots are represented
  int root_counts[12] = {};
  for (const auto& tmpl : templates) {
    root_counts[static_cast<int>(tmpl.root)]++;
  }

  for (int i = 0; i < 12; ++i) {
    REQUIRE(root_counts[i] == 9);
  }
}

TEST_CASE("generate_triad_templates", "[chord_templates]") {
  auto templates = generate_triad_templates();

  // 12 roots × 4 qualities = 48 templates
  REQUIRE(templates.size() == 48);
}

TEST_CASE("generate_seventh_templates", "[chord_templates]") {
  auto templates = generate_seventh_templates();

  // 12 roots × 3 qualities = 36 templates
  REQUIRE(templates.size() == 36);
}

TEST_CASE("ChordTemplate correlate", "[chord_templates]") {
  auto c_major = create_major_template(PitchClass::C);

  // C major chroma should correlate highly with C major template
  std::array<float, 12> c_chroma = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  float corr = c_major.correlate(c_chroma);

  // Correlation should be positive and high
  REQUIRE(corr > 0.5f);
}

TEST_CASE("find_best_chord C major", "[chord_templates]") {
  auto templates = generate_triad_templates();

  // C major chroma
  std::array<float, 12> c_chroma = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  auto [best, score] = find_best_chord(c_chroma, templates);

  REQUIRE(best.root == PitchClass::C);
  REQUIRE(best.quality == ChordQuality::Major);
  REQUIRE(score > 0.5f);
}

TEST_CASE("find_best_chord A minor", "[chord_templates]") {
  auto templates = generate_triad_templates();

  // A minor chroma (A=9, C=0, E=4)
  std::array<float, 12> am_chroma = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

  auto [best, score] = find_best_chord(am_chroma, templates);

  REQUIRE(best.root == PitchClass::A);
  REQUIRE(best.quality == ChordQuality::Minor);
  REQUIRE(score > 0.5f);
}

TEST_CASE("pitch_class_to_string", "[chord_templates]") {
  REQUIRE(pitch_class_to_string(PitchClass::C) == "C");
  REQUIRE(pitch_class_to_string(PitchClass::Cs) == "C#");
  REQUIRE(pitch_class_to_string(PitchClass::D) == "D");
  REQUIRE(pitch_class_to_string(PitchClass::A) == "A");
  REQUIRE(pitch_class_to_string(PitchClass::B) == "B");
}

TEST_CASE("chord_quality_to_string", "[chord_templates]") {
  REQUIRE(chord_quality_to_string(ChordQuality::Major) == "maj");
  REQUIRE(chord_quality_to_string(ChordQuality::Minor) == "m");
  REQUIRE(chord_quality_to_string(ChordQuality::Diminished) == "dim");
  REQUIRE(chord_quality_to_string(ChordQuality::Dominant7) == "7");
  REQUIRE(chord_quality_to_string(ChordQuality::Major7) == "maj7");
}
