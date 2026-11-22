/// @file chord_hmm_test.cpp
/// @brief Tests for the chord HMM decoder's candidate ordering.

#include "analysis/chord_hmm.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

namespace {

std::vector<ChordTemplate> major_templates(int count) {
  std::vector<ChordTemplate> templates;
  for (int root = 0; root < count; ++root) {
    templates.push_back(create_chord_template(static_cast<PitchClass>(root), ChordQuality::Major));
  }
  return templates;
}

}  // namespace

TEST_CASE("Chord HMM breaks a score tie on the lowest template index", "[chord_hmm][chord]") {
  const std::vector<ChordTemplate> templates = major_templates(6);
  ChordHmmObservation observation;
  observation.candidates = {{5, 0.5f}, {3, 0.5f}, {1, 0.5f}};

  ChordHmmConfig config;
  REQUIRE(viterbi_chord_sequence({observation}, templates, config) == std::vector<int>{1});

  // The beam keeps the same tied candidate the decoder would pick.
  config.beam_width = 1;
  REQUIRE(viterbi_chord_sequence({observation}, templates, config) == std::vector<int>{1});
}

TEST_CASE("Chord HMM tie order does not depend on candidate input order", "[chord_hmm][chord]") {
  const std::vector<ChordTemplate> templates = major_templates(6);
  ChordHmmObservation forward;
  ChordHmmObservation reversed;
  for (int i = 0; i < 6; ++i) {
    forward.candidates.emplace_back(i, 0.25f);
    reversed.candidates.emplace_back(5 - i, 0.25f);
  }
  ChordHmmConfig config;
  config.beam_width = 3;
  const std::vector<int> a = viterbi_chord_sequence({forward, forward}, templates, config);
  const std::vector<int> b = viterbi_chord_sequence({reversed, reversed}, templates, config);
  REQUIRE(a == b);
  REQUIRE(a == std::vector<int>{0, 0});
}
