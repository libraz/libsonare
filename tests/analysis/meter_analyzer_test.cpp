/// @file meter_analyzer_test.cpp
/// @brief Tests for multi-comb meter analyzer.

#include "analysis/meter_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

namespace {

std::vector<Beat> make_beats(int numerator, int measures) {
  std::vector<Beat> beats;
  beats.reserve(static_cast<size_t>(numerator * measures));
  for (int i = 0; i < numerator * measures; ++i) {
    const int pos = i % numerator;
    float strength = 0.35f;
    if (pos == 0) {
      strength = 1.0f;
    } else if ((numerator == 4 && pos == 2) || (numerator == 6 && pos == 3)) {
      strength = 0.65f;
    }
    beats.push_back({static_cast<float>(i) * 0.5f, i * 10, strength});
  }
  return beats;
}

}  // namespace

TEST_CASE("MeterAnalyzer detects 4/4 from accented beats", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  MeterAnalyzer analyzer({}, beats);

  REQUIRE(analyzer.time_signature().numerator == 4);
  REQUIRE(analyzer.time_signature().denominator == 4);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}

TEST_CASE("MeterAnalyzer detects 3/4 from accented beats", "[meter_analyzer]") {
  const auto beats = make_beats(3, 8);
  MeterAnalyzer analyzer({}, beats);

  REQUIRE(analyzer.time_signature().numerator == 3);
  REQUIRE(analyzer.time_signature().denominator == 4);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}

TEST_CASE("MeterAnalyzer detects 6/8-style compound meter", "[meter_analyzer]") {
  const auto beats = make_beats(6, 8);
  MeterAnalyzer analyzer({}, beats);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}

TEST_CASE("MeterAnalyzer numerator is stable under candidate ordering", "[meter_analyzer]") {
  // make_beats(6, 8) is a strong, unambiguous compound-6 pattern: the primary
  // selection lands on numerator 6 as the unique winner regardless of candidate
  // ordering. A non-empty onset envelope plus a sky-high compound threshold
  // disables the 6/8 promotion, forcing the 6-vs-(3|4) fallback. That fallback
  // now resolves by value-based score lookup, so the chosen numerator must be
  // identical for any candidate_numerators ordering (ITEM 3 regression guard).
  const auto beats = make_beats(6, 8);
  const std::vector<float> onset_strength(500, 0.0f);

  MeterConfig config_default;
  config_default.candidate_numerators = {3, 4, 6};
  config_default.compound_subdivision_threshold = 2.0f;  // never promote to /8

  MeterConfig config_reordered = config_default;
  config_reordered.candidate_numerators = {6, 4, 3};

  MeterAnalyzer analyzer_default(onset_strength, beats, config_default);
  MeterAnalyzer analyzer_reordered(onset_strength, beats, config_reordered);

  REQUIRE(analyzer_default.time_signature().numerator ==
          analyzer_reordered.time_signature().numerator);
}

TEST_CASE("MeterAnalyzer promotes 3-beat compound subdivisions to 6/8", "[meter_analyzer]") {
  std::vector<Beat> beats;
  std::vector<float> onset_strength(160, 0.0f);
  for (int i = 0; i < 24; ++i) {
    const int frame = i * 6;
    beats.push_back({static_cast<float>(i) * 0.5f, frame, i % 3 == 0 ? 1.0f : 0.6f});
    onset_strength[static_cast<size_t>(frame)] = 1.0f;
    onset_strength[static_cast<size_t>(frame + 3)] = 0.95f;
  }

  MeterAnalyzer analyzer(onset_strength, beats);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}
