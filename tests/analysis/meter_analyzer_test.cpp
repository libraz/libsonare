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
  REQUIRE_FALSE(analyzer.result().candidates.empty());
  REQUIRE(analyzer.result().candidates.front().numerator == 4);
  REQUIRE(analyzer.result().candidates.front().denominator == 4);
}

TEST_CASE("MeterAnalyzer normalizes onset strengths above unity", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  std::vector<float> unit_onsets(400, 0.0f);
  std::vector<float> scaled_onsets(400, 0.0f);
  for (const auto& beat : beats) {
    unit_onsets[static_cast<size_t>(beat.frame)] = beat.strength;
    scaled_onsets[static_cast<size_t>(beat.frame)] = beat.strength * 20.0f;
  }

  MeterAnalyzer unit(unit_onsets, beats);
  MeterAnalyzer scaled(scaled_onsets, beats);

  REQUIRE(scaled.time_signature().numerator == unit.time_signature().numerator);
  REQUIRE(scaled.time_signature().denominator == unit.time_signature().denominator);
  REQUIRE(scaled.result().candidate_scores == unit.result().candidate_scores);
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

TEST_CASE("MeterAnalyzer retains its existing candidate signatures", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  MeterAnalyzer analyzer({}, beats);

  REQUIRE(analyzer.result().candidates.size() == 3);
  REQUIRE(analyzer.result().candidates.front().numerator == 4);
  REQUIRE(analyzer.result().candidates.front().confidence >= 0.0f);
}

TEST_CASE("MeterAnalyzer detects an odd meter only when it is a candidate", "[meter_analyzer]") {
  for (int numerator : {5, 7}) {
    const auto beats = make_beats(numerator, 5);

    MeterConfig widened;
    widened.candidate_numerators = {3, 4, 5, 6, 7};
    MeterAnalyzer widened_analyzer({}, beats, widened);

    CAPTURE(numerator, widened_analyzer.result().candidate_scores);
    REQUIRE(widened_analyzer.time_signature().numerator == numerator);
    REQUIRE(widened_analyzer.time_signature().denominator == 4);
    REQUIRE(widened_analyzer.time_signature().confidence > 0.5f);
    REQUIRE(widened_analyzer.result().downbeat_phase == 0);

    // The same accented beats with the default candidate set cannot report the
    // odd numerator, so the detection above comes from the config rather than
    // from the pattern alone.
    MeterAnalyzer default_analyzer({}, beats);
    const int default_numerator = default_analyzer.time_signature().numerator;
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != numerator);
    REQUIRE((default_numerator == 3 || default_numerator == 4 || default_numerator == 6));
  }
}

TEST_CASE("MeterAnalyzer reports the requested beat unit", "[meter_analyzer]") {
  // A 4-beat accent pattern never reaches the compound branch, which is the one
  // place the estimator overrides the requested unit.
  const auto beats = make_beats(4, 8);

  MeterConfig eighth;
  eighth.denominator = 8;
  MeterAnalyzer eighth_analyzer({}, beats, eighth);
  REQUIRE(eighth_analyzer.time_signature().numerator == 4);
  REQUIRE(eighth_analyzer.time_signature().denominator == 8);

  MeterConfig half;
  half.denominator = 2;
  MeterAnalyzer half_analyzer({}, beats, half);
  REQUIRE(half_analyzer.time_signature().numerator == 4);
  REQUIRE(half_analyzer.time_signature().denominator == 2);

  // The candidate list carries the same unit, except for the compound-6 entry
  // the estimator reports in eighths on its own.
  for (const auto& candidate : half_analyzer.result().candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    REQUIRE(candidate.denominator == (candidate.numerator == 6 ? 8 : 2));
  }
}

TEST_CASE("MeterAnalyzer keeps the compound beat unit over the requested one", "[meter_analyzer]") {
  // Documented exception to the requested-unit rule: a resolved compound meter
  // is reported in eighths whatever the config asked for.
  const auto beats = make_beats(6, 8);

  MeterConfig config;
  config.denominator = 2;
  MeterAnalyzer analyzer({}, beats, config);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
}

TEST_CASE("MeterAnalyzer reports an odd meter in the requested unit", "[meter_analyzer]") {
  // The compound override is keyed on numerator 6, so an odd meter keeps the
  // requested unit rather than being promoted to eighths.
  const auto beats = make_beats(5, 5);

  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6};
  config.denominator = 8;
  MeterAnalyzer analyzer({}, beats, config);

  REQUIRE(analyzer.time_signature().numerator == 5);
  REQUIRE(analyzer.time_signature().denominator == 8);
}

TEST_CASE("MeterAnalyzer phase stays inside the reported numerator", "[meter_analyzer]") {
  // A 6-accent pattern whose first downbeat is at beat 4 scores best as a
  // 6-comb at phase 4. When the weak compound evidence sends the result back to
  // a simple meter, the phase has to follow the numerator that is actually
  // reported — a phase of 4 against a numerator of 3 addresses no beat of the
  // first measure and shifts every bar position downstream.
  std::vector<Beat> beats;
  const int total = 36;
  beats.reserve(static_cast<size_t>(total));
  std::vector<float> onset_strength(static_cast<size_t>(total) * 10, 0.05f);
  for (int i = 0; i < total; ++i) {
    const bool downbeat = ((i - 4) % 6 + 6) % 6 == 0;
    const float strength = downbeat ? 1.0f : 0.3f;
    beats.push_back({static_cast<float>(i) * 0.5f, i * 10, strength});
    onset_strength[static_cast<size_t>(i * 10)] = strength;
  }

  const MeterResult result = estimate_meter(onset_strength, beats, MeterConfig());

  // Precondition, not the property under test: the range check below is only
  // meaningful while this fixture still takes the simple-meter fallback. A
  // reported 6 would satisfy `phase < numerator` for the rejected phase too.
  INFO("reported " << result.time_signature.numerator << "/" << result.time_signature.denominator
                   << " phase " << result.downbeat_phase);
  REQUIRE(result.time_signature.numerator != 6);

  REQUIRE(result.downbeat_phase >= 0);
  REQUIRE(result.downbeat_phase < result.time_signature.numerator);
}
