/// @file chord_templates_test.cpp
/// @brief Tests for chord templates.

#include "analysis/chord_templates.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

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

TEST_CASE("create_extended_chord_templates", "[chord_templates]") {
  auto c_add9 = create_add9_template(PitchClass::C);
  REQUIRE(c_add9.quality == ChordQuality::Add9);
  REQUIRE_THAT(c_add9.pattern[0], WithinAbs(1.0f, 0.001f));  // C
  REQUIRE_THAT(c_add9.pattern[2], WithinAbs(1.0f, 0.001f));  // D
  REQUIRE_THAT(c_add9.pattern[4], WithinAbs(1.0f, 0.001f));  // E
  REQUIRE_THAT(c_add9.pattern[7], WithinAbs(1.0f, 0.001f));  // G

  auto b_half_dim = create_half_dim7_template(PitchClass::B);
  REQUIRE(b_half_dim.quality == ChordQuality::HalfDim7);
  REQUIRE_THAT(b_half_dim.pattern[11], WithinAbs(1.0f, 0.001f));  // B
  REQUIRE_THAT(b_half_dim.pattern[2], WithinAbs(1.0f, 0.001f));   // D
  REQUIRE_THAT(b_half_dim.pattern[5], WithinAbs(1.0f, 0.001f));   // F
  REQUIRE_THAT(b_half_dim.pattern[9], WithinAbs(1.0f, 0.001f));   // A

  auto g9 = create_dominant9_template(PitchClass::G);
  REQUIRE(g9.quality == ChordQuality::Dominant9);
  REQUIRE_THAT(g9.pattern[7], WithinAbs(1.0f, 0.001f));   // G
  REQUIRE_THAT(g9.pattern[9], WithinAbs(1.0f, 0.001f));   // A
  REQUIRE_THAT(g9.pattern[11], WithinAbs(1.0f, 0.001f));  // B
  REQUIRE_THAT(g9.pattern[2], WithinAbs(1.0f, 0.001f));   // D
  REQUIRE_THAT(g9.pattern[5], WithinAbs(1.0f, 0.001f));   // F
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

  auto d_add9 = create_add9_template(PitchClass::D);
  REQUIRE(d_add9.to_string() == "Dadd9");

  auto b_half_dim = create_half_dim7_template(PitchClass::B);
  REQUIRE(b_half_dim.to_string() == "Bm7b5");
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

  // 16 base qualities plus the extended vocabulary, at each of the 12 roots.
  const size_t qualities_per_root = 16 + extended_chord_qualities().size();
  REQUIRE(templates.size() == 12 * qualities_per_root);

  // Check that all roots are represented
  int root_counts[12] = {};
  for (const auto& tmpl : templates) {
    root_counts[static_cast<int>(tmpl.root)]++;
  }

  for (int i = 0; i < 12; ++i) {
    REQUIRE(root_counts[i] == static_cast<int>(qualities_per_root));
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

TEST_CASE("find_best_chord extended qualities", "[chord_templates]") {
  auto templates = generate_all_chord_templates();

  std::array<float, 12> c_add9 = {1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  auto [best_add9, add9_score] = find_best_chord(c_add9, templates);
  REQUIRE(best_add9.root == PitchClass::C);
  REQUIRE(best_add9.quality == ChordQuality::Add9);
  REQUIRE(add9_score > 0.5f);

  std::array<float, 12> b_half_dim = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
  auto [best_half_dim, half_dim_score] = find_best_chord(b_half_dim, templates);
  REQUIRE(best_half_dim.root == PitchClass::B);
  REQUIRE(best_half_dim.quality == ChordQuality::HalfDim7);
  REQUIRE(half_dim_score > 0.5f);
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
  REQUIRE(chord_quality_to_string(ChordQuality::Add9) == "add9");
  REQUIRE(chord_quality_to_string(ChordQuality::HalfDim7) == "m7b5");
  REQUIRE(chord_quality_to_string(ChordQuality::Dominant9) == "9");
}

TEST_CASE("extended chord qualities spell their conventional voicings", "[chord_templates]") {
  auto pitch_classes = [](ChordQuality quality) {
    const ChordIntervals intervals = chord_quality_intervals(quality);
    std::vector<int> degrees;
    for (size_t i = 0; i < intervals.count; ++i) {
      degrees.push_back(intervals.semitones[i] % 12);
    }
    std::sort(degrees.begin(), degrees.end());
    return degrees;
  };

  REQUIRE(pitch_classes(ChordQuality::Major6) == std::vector<int>{0, 4, 7, 9});
  REQUIRE(pitch_classes(ChordQuality::Minor6) == std::vector<int>{0, 3, 7, 9});
  REQUIRE(pitch_classes(ChordQuality::MinorMajor7) == std::vector<int>{0, 3, 7, 11});
  REQUIRE(pitch_classes(ChordQuality::Dominant7Sus4) == std::vector<int>{0, 5, 7, 10});
  // The eleventh omits the third, which would clash a semitone below it; the
  // thirteenth omits the fifth, which is what a five-voice 13th drops first.
  REQUIRE(pitch_classes(ChordQuality::Dominant11) == std::vector<int>{0, 2, 5, 7, 10});
  REQUIRE(pitch_classes(ChordQuality::Dominant13) == std::vector<int>{0, 2, 4, 9, 10});
  REQUIRE(pitch_classes(ChordQuality::Dominant7b9) == std::vector<int>{0, 1, 4, 7, 10});
  REQUIRE(pitch_classes(ChordQuality::Dominant7s9) == std::vector<int>{0, 3, 4, 7, 10});
}

TEST_CASE("three extended qualities are anagrams of a commoner one", "[chord_templates]") {
  // The pairs the recogniser cannot separate on chroma alone. Pinning them here
  // records which ambiguities are structural, so a later change that "fixes" one
  // of them has to explain what new evidence it found.
  struct Anagram {
    PitchClass extended_root;
    ChordQuality extended;
    PitchClass common_root;
    ChordQuality common;
  };
  const Anagram pairs[] = {
      {PitchClass::C, ChordQuality::Major6, PitchClass::A, ChordQuality::Minor7},
      {PitchClass::D, ChordQuality::Minor6, PitchClass::B, ChordQuality::HalfDim7},
      {PitchClass::G, ChordQuality::Dominant7Sus4, PitchClass::C, ChordQuality::Sus2Add4},
  };
  for (const Anagram& pair : pairs) {
    CAPTURE(chord_quality_to_string(pair.extended));
    REQUIRE(
        chord_pitch_class_mask(static_cast<int>(pair.extended_root),
                               static_cast<int>(pair.extended)) ==
        chord_pitch_class_mask(static_cast<int>(pair.common_root), static_cast<int>(pair.common)));
  }
}

TEST_CASE("chord_quality_triad_base reads the third out of the interval table",
          "[chord_templates]") {
  REQUIRE(chord_quality_triad_base(ChordQuality::Major6) == ChordQuality::Major);
  REQUIRE(chord_quality_triad_base(ChordQuality::Dominant13) == ChordQuality::Major);
  REQUIRE(chord_quality_triad_base(ChordQuality::Minor6) == ChordQuality::Minor);
  REQUIRE(chord_quality_triad_base(ChordQuality::MinorMajor7) == ChordQuality::Minor);
  REQUIRE(chord_quality_triad_base(ChordQuality::Minor7) == ChordQuality::Minor);
  REQUIRE(chord_quality_triad_base(ChordQuality::HalfDim7) == ChordQuality::Diminished);
  REQUIRE(chord_quality_triad_base(ChordQuality::Dim7) == ChordQuality::Diminished);
  REQUIRE(chord_quality_triad_base(ChordQuality::Augmented) == ChordQuality::Augmented);

  // A sharp ninth is enharmonically a minor third but sits above the seventh;
  // the chord it decorates is a dominant, so the major third wins.
  REQUIRE(chord_quality_triad_base(ChordQuality::Dominant7s9) == ChordQuality::Major);

  // Qualities with no third of their own report themselves rather than
  // inventing a colour they do not have.
  REQUIRE(chord_quality_triad_base(ChordQuality::Sus4) == ChordQuality::Sus4);
  REQUIRE(chord_quality_triad_base(ChordQuality::Dominant7Sus4) == ChordQuality::Dominant7Sus4);
  REQUIRE(chord_quality_triad_base(ChordQuality::Dominant11) == ChordQuality::Dominant11);
  REQUIRE(chord_quality_triad_base(ChordQuality::Unknown) == ChordQuality::Unknown);
}

TEST_CASE("chord_quality_is_dominant_seventh requires the tritone", "[chord_templates]") {
  REQUIRE(chord_quality_is_dominant_seventh(ChordQuality::Dominant7));
  REQUIRE(chord_quality_is_dominant_seventh(ChordQuality::Dominant9));
  REQUIRE(chord_quality_is_dominant_seventh(ChordQuality::Dominant13));
  REQUIRE(chord_quality_is_dominant_seventh(ChordQuality::Dominant7b9));
  REQUIRE(chord_quality_is_dominant_seventh(ChordQuality::Dominant7s9));

  // A minor seventh without a major third has no tritone and does not pull.
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::Minor7));
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::HalfDim7));
  // Both of these omit the third, so neither is a dominant despite the flat seventh.
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::Dominant7Sus4));
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::Dominant11));
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::Major7));
  REQUIRE_FALSE(chord_quality_is_dominant_seventh(ChordQuality::Major));
}

TEST_CASE("the fifth bonus follows the chord's own fifth", "[chord_templates]") {
  // A half-diminished chord's fifth is diminished. Looking for a perfect fifth
  // meant a correctly spelled m7b5 could never collect the bonus that confirms
  // it, which is how the min6 spelling the same four notes came to beat it.
  const auto b_half_dim = create_half_dim7_template(PitchClass::B);
  std::array<float, 12> b_half_dim_chroma = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                             0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
  std::array<float, 12> without_fifth = b_half_dim_chroma;
  without_fifth[5] = 0.0f;  // remove F, the diminished fifth above B

  REQUIRE(b_half_dim.correlate(b_half_dim_chroma) > b_half_dim.correlate(without_fifth));
}
