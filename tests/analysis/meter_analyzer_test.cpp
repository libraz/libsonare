/// @file meter_analyzer_test.cpp
/// @brief Tests for multi-comb meter analyzer.

#include "analysis/meter_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "util/exception.h"

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

/// @brief Accents carrying each beat's strength plus a subdivision after every beat.
/// @details The compound reading is resolved from the mean accent at the beat
///          midpoints relative to the beats themselves, so @p subdivision_ratio
///          is what that score comes out as. A test that wants a compound meter
///          resolved has to supply one: without accents there is no subdivision
///          to measure and the estimator reports the numerator with its grouping
///          instead of promoting it. A beat with no strength holds no attack.
BeatAccents make_accents(const std::vector<Beat>& beats, float subdivision_ratio) {
  BeatAccents accents;
  for (size_t i = 0; i < beats.size(); ++i) {
    accents.beat.push_back(beats[i].strength);
    accents.beat_has_attack.push_back(beats[i].strength > 0.0f ? 1 : 0);
    if (i + 1 < beats.size()) accents.subdivision.push_back(beats[i].strength * subdivision_ratio);
  }
  return accents;
}

MeterBeats make_meter_beats(int numerator, int measures) {
  MeterBeats observations;
  for (int i = 0; i < numerator * measures; ++i) {
    const int position = i % numerator;
    float accent = 0.35f;
    if (position == 0) {
      accent = 1.0f;
    } else if ((numerator == 4 && position == 2) || (numerator == 6 && position == 3)) {
      accent = 0.65f;
    }
    observations.accent.push_back(accent);
  }
  return observations;
}

/// @brief A beat series whose bars divide exactly as @p grouping says.
/// @details The downbeat is the loudest beat, each following group starts on a
///          middling one, and the rest are quiet, which is the accent shape an
///          additive meter is notated for.
MeterBeats make_grouped_observations(const std::vector<int>& grouping, int measures) {
  std::vector<int> accent_positions;
  int position = 0;
  for (size_t i = 0; i + 1 < grouping.size(); ++i) {
    position += grouping[i];
    accent_positions.push_back(position);
  }
  const int numerator = std::accumulate(grouping.begin(), grouping.end(), 0);

  MeterBeats observations;
  for (int i = 0; i < numerator * measures; ++i) {
    const int pos = i % numerator;
    float strength = 0.35f;
    if (pos == 0) {
      strength = 1.0f;
    } else if (std::find(accent_positions.begin(), accent_positions.end(), pos) !=
               accent_positions.end()) {
      strength = 0.65f;
    }
    observations.accent.push_back(strength);
  }
  return observations;
}

}  // namespace

TEST_CASE("MeterAnalyzer detects 4/4 from accented beats", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  MeterAnalyzer analyzer(beats);

  REQUIRE(analyzer.time_signature().numerator == 4);
  REQUIRE(analyzer.time_signature().denominator == 4);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
  REQUIRE_FALSE(analyzer.result().candidates.empty());
  REQUIRE(analyzer.result().candidates.front().numerator == 4);
  REQUIRE(analyzer.result().candidates.front().denominator == 4);
}

TEST_CASE("MeterAnalyzer normalizes accents above unity", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  const BeatAccents unit_accents = make_accents(beats, 0.0f);
  BeatAccents scaled_accents = unit_accents;
  for (float& accent : scaled_accents.beat) {
    accent *= 20.0f;
  }

  MeterAnalyzer unit(beats, MeterConfig(), unit_accents);
  MeterAnalyzer scaled(beats, MeterConfig(), scaled_accents);

  REQUIRE(scaled.time_signature().numerator == unit.time_signature().numerator);
  REQUIRE(scaled.time_signature().denominator == unit.time_signature().denominator);
  REQUIRE(scaled.result().candidate_scores == unit.result().candidate_scores);
}

TEST_CASE("MeterAnalyzer detects 3/4 from accented beats", "[meter_analyzer]") {
  const auto beats = make_beats(3, 8);
  MeterAnalyzer analyzer(beats);

  REQUIRE(analyzer.time_signature().numerator == 3);
  REQUIRE(analyzer.time_signature().denominator == 4);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}

TEST_CASE("MeterAnalyzer detects 6/8-style compound meter", "[meter_analyzer]") {
  // The promotion to eighths is a claim about how a beat divides, so it needs
  // accents carrying that subdivision. The same beats without one report
  // the six with its 3+3 grouping and the requested unit.
  const auto beats = make_beats(6, 8);
  MeterAnalyzer analyzer(beats, MeterConfig(), make_accents(beats, 1.0f));

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);

  MeterAnalyzer unmeasured(beats);
  REQUIRE(unmeasured.time_signature().numerator == 6);
  REQUIRE(unmeasured.time_signature().denominator == 4);
  REQUIRE(unmeasured.result().grouping == std::vector<int>{3, 3});
}

TEST_CASE("MeterAnalyzer numerator is stable under candidate ordering", "[meter_analyzer]") {
  // make_beats(6, 8) is a strong, unambiguous compound-6 pattern: the primary
  // selection lands on numerator 6 as the unique winner regardless of candidate
  // ordering. A measured but empty subdivision plus a sky-high compound threshold
  // disables the 6/8 promotion, forcing the 6-vs-(3|4) fallback. That fallback
  // now resolves by value-based score lookup, so the chosen numerator must be
  // identical for any candidate_numerators ordering (ITEM 3 regression guard).
  const auto beats = make_beats(6, 8);
  const BeatAccents accents = make_accents(beats, 0.0f);

  MeterConfig config_default;
  config_default.candidate_numerators = {3, 4, 6};
  config_default.compound_subdivision_threshold = 2.0f;  // never promote to /8

  MeterConfig config_reordered = config_default;
  config_reordered.candidate_numerators = {6, 4, 3};

  MeterAnalyzer analyzer_default(beats, config_default, accents);
  MeterAnalyzer analyzer_reordered(beats, config_reordered, accents);

  REQUIRE(analyzer_default.time_signature().numerator ==
          analyzer_reordered.time_signature().numerator);
}

TEST_CASE("MeterAnalyzer promotes 3-beat compound subdivisions to 6/8", "[meter_analyzer]") {
  // Every beat reads alike in level and each one is followed by a subdivision nearly as strong.
  std::vector<Beat> beats;
  BeatAccents accents;
  for (int i = 0; i < 24; ++i) {
    beats.push_back({static_cast<float>(i) * 0.5f, i * 6, i % 3 == 0 ? 1.0f : 0.6f});
    accents.beat.push_back(1.0f);
    accents.beat_has_attack.push_back(1);
    if (i + 1 < 24) accents.subdivision.push_back(0.95f);
  }

  MeterAnalyzer analyzer(beats, MeterConfig(), accents);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);
}

TEST_CASE("MeterAnalyzer retains its existing candidate signatures", "[meter_analyzer]") {
  const auto beats = make_beats(4, 8);
  MeterAnalyzer analyzer(beats);

  REQUIRE(analyzer.result().candidates.size() == 3);
  REQUIRE(analyzer.result().candidates.front().numerator == 4);
  REQUIRE(analyzer.result().candidates.front().confidence >= 0.0f);
}

TEST_CASE("MeterAnalyzer detects an odd meter only when it is a candidate", "[meter_analyzer]") {
  for (int numerator : {5, 7}) {
    const auto beats = make_beats(numerator, 5);

    MeterConfig widened;
    widened.candidate_numerators = {3, 4, 5, 6, 7};
    MeterAnalyzer widened_analyzer(beats, widened);

    CAPTURE(numerator, widened_analyzer.result().candidate_scores);
    REQUIRE(widened_analyzer.time_signature().numerator == numerator);
    REQUIRE(widened_analyzer.time_signature().denominator == 4);
    REQUIRE(widened_analyzer.time_signature().confidence > 0.5f);
    REQUIRE(widened_analyzer.result().downbeat_phase == 0);

    // The same accented beats with the default candidate set cannot report the
    // odd numerator, so the detection above comes from the config rather than
    // from the pattern alone.
    MeterAnalyzer default_analyzer(beats);
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
  MeterAnalyzer eighth_analyzer(beats, eighth);
  REQUIRE(eighth_analyzer.time_signature().numerator == 4);
  REQUIRE(eighth_analyzer.time_signature().denominator == 8);

  MeterConfig half;
  half.denominator = 2;
  MeterAnalyzer half_analyzer(beats, half);
  REQUIRE(half_analyzer.time_signature().numerator == 4);
  REQUIRE(half_analyzer.time_signature().denominator == 2);

  // The candidate list carries the requested unit throughout: these beats are a
  // four, so nothing in them divides into two groups of three and no candidate
  // earns the compound reading that is the one exception to the requested unit.
  for (const auto& candidate : half_analyzer.result().candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    REQUIRE(candidate.denominator == 2);
  }

  // The same candidate on beats that do divide into two threes, with the
  // subdivision there to measure, takes the eighth -- which is what keeps the
  // loop above from passing vacuously.
  const auto six_beats = make_beats(6, 8);
  MeterAnalyzer compound_analyzer(six_beats, half, make_accents(six_beats, 1.0f));
  const auto& compound_candidates = compound_analyzer.result().candidates;
  const auto six = std::find_if(compound_candidates.begin(), compound_candidates.end(),
                                [](const TimeSignature& c) { return c.numerator == 6; });
  REQUIRE(six != compound_candidates.end());
  REQUIRE(six->denominator == 8);
}

TEST_CASE("MeterAnalyzer keeps the compound beat unit over the requested one", "[meter_analyzer]") {
  // Documented exception to the requested-unit rule: a compound meter that was
  // actually resolved is reported in eighths whatever the config asked for.
  // "Resolved" is the operative word -- it takes a measured subdivision, not
  // just a 3+3 accent pattern.
  const auto beats = make_beats(6, 8);

  MeterConfig config;
  config.denominator = 2;
  MeterAnalyzer analyzer(beats, config, make_accents(beats, 1.0f));

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);

  // Without the subdivision the exception does not apply and the requested
  // unit stands.
  MeterAnalyzer unmeasured(beats, config);
  REQUIRE(unmeasured.time_signature().numerator == 6);
  REQUIRE(unmeasured.time_signature().denominator == 2);
}

TEST_CASE("MeterAnalyzer reports an odd meter in the requested unit", "[meter_analyzer]") {
  // The compound override is keyed on numerator 6, so an odd meter keeps the
  // requested unit rather than being promoted to eighths.
  const auto beats = make_beats(5, 5);

  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6};
  config.denominator = 8;
  MeterAnalyzer analyzer(beats, config);

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
  BeatAccents accents;
  for (int i = 0; i < total; ++i) {
    const bool downbeat = ((i - 4) % 6 + 6) % 6 == 0;
    const float strength = downbeat ? 1.0f : 0.3f;
    beats.push_back({static_cast<float>(i) * 0.5f, i * 10, strength});
    accents.beat.push_back(strength);
    accents.beat_has_attack.push_back(1);
    if (i + 1 < total) accents.subdivision.push_back(0.05f);
  }

  const MeterResult result = estimate_meter(beats, MeterConfig(), accents);

  // Precondition, not the property under test: the range check below is only
  // meaningful while this fixture still takes the simple-meter fallback. A
  // reported 6 would satisfy `phase < numerator` for the rejected phase too.
  INFO("reported " << result.time_signature.numerator << "/" << result.time_signature.denominator
                   << " phase " << result.downbeat_phase);
  REQUIRE(result.time_signature.numerator != 6);

  REQUIRE(result.downbeat_phase >= 0);
  REQUIRE(result.downbeat_phase < result.time_signature.numerator);
}

TEST_CASE("validate_meter_config accepts the defaults and the inclusive range ends",
          "[meter_analyzer]") {
  REQUIRE_NOTHROW(validate_meter_config(MeterConfig()));

  const auto accepts = [](void (*mutate)(MeterConfig&)) {
    MeterConfig config;
    mutate(config);
    REQUIRE_NOTHROW(validate_meter_config(config));
  };

  accepts([](MeterConfig& c) { c.candidate_numerators.assign(kMaxMeterCandidateNumerators, 4); });
  accepts([](MeterConfig& c) {
    c.candidate_numerators = {kMinMeterCandidateNumerator, kMaxMeterCandidateNumerator};
  });
  accepts([](MeterConfig& c) { c.denominator = 1; });
  accepts([](MeterConfig& c) { c.denominator = kMaxMeterDenominator; });
  // Zero is a legal weight — it switches a scoring term off rather than being
  // out of range — so the guards cannot be tightened to "positive".
  accepts([](MeterConfig& c) { c.downbeat_weight = 0.0f; });
  accepts([](MeterConfig& c) { c.measure_weight = 0.0f; });
  accepts([](MeterConfig& c) { c.subdivision_weight = 0.0f; });
  accepts([](MeterConfig& c) { c.compound_subdivision_threshold = 0.0f; });
}

TEST_CASE("validate_meter_config rejects a config the estimator cannot answer",
          "[meter_analyzer]") {
  const auto rejects = [](void (*mutate)(MeterConfig&)) {
    MeterConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(validate_meter_config(config), SonareException);
  };

  // An empty list would return a fixed low-confidence 4/4 that reads like a
  // detection result.
  rejects([](MeterConfig& c) { c.candidate_numerators.clear(); });
  rejects(
      [](MeterConfig& c) { c.candidate_numerators.assign(kMaxMeterCandidateNumerators + 1, 4); });
  rejects([](MeterConfig& c) { c.candidate_numerators = {kMinMeterCandidateNumerator - 1}; });
  rejects([](MeterConfig& c) { c.candidate_numerators = {kMaxMeterCandidateNumerator + 1}; });
  // Every entry is checked, not only the first.
  rejects([](MeterConfig& c) { c.candidate_numerators = {4, 1}; });
  rejects([](MeterConfig& c) { c.candidate_numerators = {4, 33}; });
  rejects([](MeterConfig& c) { c.candidate_numerators = {4, 0}; });
  rejects([](MeterConfig& c) { c.candidate_numerators = {4, -3}; });

  // Only a power of two is a note value.
  rejects([](MeterConfig& c) { c.denominator = 3; });
  rejects([](MeterConfig& c) { c.denominator = 6; });
  rejects([](MeterConfig& c) { c.denominator = 12; });
  rejects([](MeterConfig& c) { c.denominator = 0; });
  rejects([](MeterConfig& c) { c.denominator = -4; });
  // A power of two past the documented ceiling is still out of range.
  rejects([](MeterConfig& c) { c.denominator = kMaxMeterDenominator * 2; });

  rejects([](MeterConfig& c) { c.downbeat_weight = -1.0f; });
  rejects([](MeterConfig& c) { c.measure_weight = -0.001f; });
  rejects([](MeterConfig& c) { c.subdivision_weight = std::nanf(""); });
  rejects([](MeterConfig& c) { c.downbeat_weight = -std::numeric_limits<float>::infinity(); });
  rejects([](MeterConfig& c) {
    c.compound_subdivision_threshold = std::numeric_limits<float>::infinity();
  });
  rejects([](MeterConfig& c) { c.compound_subdivision_threshold = -0.5f; });
}

TEST_CASE("estimate_meter_from_observations applies config guards before scoring",
          "[meter_analyzer]") {
  const MeterBeats observations = make_meter_beats(4, 8);

  MeterConfig empty_candidates;
  empty_candidates.candidate_numerators.clear();
  REQUIRE_THROWS_AS(estimate_meter_from_observations(observations, empty_candidates),
                    SonareException);

  MeterConfig odd_denominator;
  odd_denominator.denominator = 3;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(observations, odd_denominator),
                    SonareException);

  MeterConfig negative_weight;
  negative_weight.measure_weight = -1.0f;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(observations, negative_weight),
                    SonareException);
}

TEST_CASE("estimate_meter_from_observations validates evidence shape and values",
          "[meter_analyzer]") {
  MeterConfig config;
  MeterBeats observations = make_meter_beats(4, 8);
  REQUIRE_NOTHROW(estimate_meter_from_observations(observations, config));

  MeterBeats no_accents;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(no_accents, config), SonareException);

  MeterBeats wrong_attacks = observations;
  wrong_attacks.has_attack = {1};
  REQUIRE_THROWS_AS(estimate_meter_from_observations(wrong_attacks, config), SonareException);

  MeterBeats wrong_subdivision = observations;
  wrong_subdivision.subdivision_accent.assign(observations.accent.size(), 0.0f);
  REQUIRE_THROWS_AS(estimate_meter_from_observations(wrong_subdivision, config), SonareException);

  MeterBeats negative_accent = observations;
  negative_accent.accent[2] = -1.0f;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(negative_accent, config), SonareException);

  MeterBeats non_finite_energy = observations;
  non_finite_energy.low_frequency_energy.assign(observations.accent.size(),
                                                std::numeric_limits<float>::infinity());
  REQUIRE_THROWS_AS(estimate_meter_from_observations(non_finite_energy, config), SonareException);

  MeterBeats invalid_flag = observations;
  invalid_flag.has_attack.assign(observations.accent.size(), 1);
  invalid_flag.has_attack.front() = 2;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(invalid_flag, config), SonareException);
}

TEST_CASE("estimate_meter_from_observations distinguishes empty from short evidence",
          "[meter_analyzer]") {
  MeterConfig config;
  MeterBeats empty;
  REQUIRE_THROWS_AS(estimate_meter_from_observations(empty, config), SonareException);

  MeterBeats one_beat;
  one_beat.accent = {1.0f};
  const MeterResult one = estimate_meter_from_observations(one_beat, config);
  REQUIRE_FALSE(one.searched);
  REQUIRE(one.time_signature.numerator == 4);
  REQUIRE(one.time_signature.denominator == 4);
  REQUIRE(one.downbeat_phase == 0);
  REQUIRE(one.candidates.size() == 1);
  REQUIRE(one.time_signature.confidence == 0.0f);

  const MeterBeats full = make_meter_beats(4, 8);
  const MeterResult searched = estimate_meter_from_observations(full, config);
  REQUIRE(searched.time_signature.confidence > one.time_signature.confidence);
}

TEST_CASE("estimate_meter_from_observations reports the fallback below the search floor",
          "[meter_analyzer]") {
  MeterConfig config;
  const MeterBeats full = make_meter_beats(4, 8);
  const MeterResult searched = estimate_meter_from_observations(full, config);
  REQUIRE(searched.searched);
  REQUIRE(searched.time_signature.confidence > 0.5f);

  for (int count = 1; count < 8; ++count) {
    MeterBeats short_evidence;
    short_evidence.accent.assign(full.accent.begin(), full.accent.begin() + count);
    const MeterResult result = estimate_meter_from_observations(short_evidence, config);

    CAPTURE(count, result.time_signature.numerator, result.time_signature.confidence);
    REQUIRE_FALSE(result.searched);
    REQUIRE(result.time_signature.numerator == 4);
    REQUIRE(result.time_signature.denominator == 4);
    REQUIRE(result.downbeat_phase == 0);
    REQUIRE(result.candidates.size() == 1);
    REQUIRE(std::all_of(result.candidate_scores.begin(), result.candidate_scores.end(),
                        [](float score) { return score == 0.0f; }));
    REQUIRE(result.time_signature.confidence == 0.0f);
    REQUIRE(result.candidates.front().confidence == 0.0f);
  }
}

TEST_CASE("estimate_meter_from_observations reads compound evidence only when measured",
          "[meter_analyzer]") {
  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};
  MeterBeats observations = make_grouped_observations({3, 3}, 16);

  const MeterResult absent = estimate_meter_from_observations(observations, config);
  REQUIRE(absent.searched);
  REQUIRE(absent.time_signature.numerator == 6);
  REQUIRE(absent.time_signature.denominator == 4);
  REQUIRE(absent.grouping == std::vector<int>{3, 3});
  REQUIRE_FALSE(absent.compound_score.has_value());
  for (const TimeSignature& candidate : absent.candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    if (candidate.numerator == 6) REQUIRE(candidate.denominator == 4);
  }

  MeterConfig eighths = config;
  eighths.denominator = 8;
  const MeterResult in_eighths = estimate_meter_from_observations(observations, eighths);
  REQUIRE(in_eighths.time_signature.numerator == 6);
  REQUIRE(in_eighths.time_signature.denominator == 8);
  REQUIRE(in_eighths.grouping == std::vector<int>{3, 3});

  MeterConfig permissive = config;
  permissive.compound_subdivision_threshold = 0.0f;
  const MeterResult without_subdivisions =
      estimate_meter_from_observations(observations, permissive);
  REQUIRE(without_subdivisions.time_signature.denominator == 4);
  REQUIRE_FALSE(without_subdivisions.compound_score.has_value());

  observations.subdivision_accent.assign(observations.accent.size() - 1, 0.0f);
  const MeterResult measured_zero = estimate_meter_from_observations(observations, config);
  REQUIRE(measured_zero.time_signature.denominator == 4);
  REQUIRE(measured_zero.compound_score.has_value());
  REQUIRE(*measured_zero.compound_score == 0.0f);

  std::fill(observations.subdivision_accent.begin(), observations.subdivision_accent.end(), 0.8f);
  const MeterResult measured = estimate_meter_from_observations(observations, config);
  REQUIRE(measured.time_signature.numerator == 6);
  REQUIRE(measured.time_signature.denominator == 8);
  REQUIRE(measured.compound_score.has_value());
  REQUIRE(*measured.compound_score > 0.5f);
}

TEST_CASE("meter scores grow with the length of the evidence span", "[meter_analyzer]") {
  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};

  const auto top_score = [&config](int bars) {
    const MeterBeats observations = make_meter_beats(4, bars);
    const MeterResult result = estimate_meter_from_observations(observations, config);
    REQUIRE(result.searched);
    REQUIRE(result.time_signature.numerator == 4);
    return *std::max_element(result.candidate_scores.begin(), result.candidate_scores.end());
  };

  const float short_span = top_score(4);
  const float long_span = top_score(16);
  CAPTURE(short_span, long_span);
  REQUIRE(long_span > short_span * 1.5f);
  REQUIRE(long_span < short_span * 2.5f);
}

TEST_CASE("estimate_meter_from_observations detects 4/4 from accents", "[meter_analyzer]") {
  const MeterBeats observations = make_meter_beats(4, 8);
  const MeterConfig config;
  const MeterResult result = estimate_meter_from_observations(observations, config);

  REQUIRE(result.time_signature.numerator == 4);
  REQUIRE(result.time_signature.denominator == 4);
  REQUIRE(result.time_signature.confidence > 0.5f);
  REQUIRE(result.downbeat_phase == 0);
  REQUIRE(result.candidate_scores.size() == config.candidate_numerators.size());
  REQUIRE_FALSE(result.candidates.empty());
  REQUIRE(result.candidates.front().numerator == 4);
  REQUIRE(result.candidates.front().denominator == 4);

  const MeterResult direct = estimate_meter(make_beats(4, 8), config);
  REQUIRE(result.time_signature.numerator == direct.time_signature.numerator);
  REQUIRE(result.time_signature.denominator == direct.time_signature.denominator);
  REQUIRE(result.time_signature.confidence == direct.time_signature.confidence);
  REQUIRE(result.downbeat_phase == direct.downbeat_phase);
  REQUIRE(result.candidate_scores == direct.candidate_scores);
  REQUIRE(result.candidates.size() == direct.candidates.size());
}

TEST_CASE("meter scores depend on accent contrast rather than absolute level", "[meter_analyzer]") {
  const MeterBeats unit = make_meter_beats(4, 8);
  MeterBeats scaled = unit;
  for (float& accent : scaled.accent) accent *= 20.0f;

  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6, 7};
  const MeterResult unit_result = estimate_meter_from_observations(unit, config);
  const MeterResult scaled_result = estimate_meter_from_observations(scaled, config);

  REQUIRE(scaled_result.time_signature.numerator == 4);
  REQUIRE(scaled_result.time_signature.numerator == unit_result.time_signature.numerator);
  REQUIRE(scaled_result.time_signature.denominator == unit_result.time_signature.denominator);
  REQUIRE(scaled_result.time_signature.confidence == unit_result.time_signature.confidence);
  REQUIRE(scaled_result.downbeat_phase == unit_result.downbeat_phase);
  REQUIRE(scaled_result.candidate_scores == unit_result.candidate_scores);

  const auto& scores = scaled_result.candidate_scores;
  const size_t best = static_cast<size_t>(
      std::distance(scores.begin(), std::max_element(scores.begin(), scores.end())));
  REQUIRE(config.candidate_numerators[best] == 4);
  for (size_t i = 0; i < scores.size(); ++i) {
    if (i != best) REQUIRE(scores[i] < scores[best]);
  }
}

TEST_CASE("meter does not favour a wide numerator on unaccented patterns", "[meter_analyzer]") {
  const std::vector<int> numerators = {3, 4, 5, 7, 9, 11, 13};
  MeterConfig config;
  config.candidate_numerators = numerators;

  std::map<int, int> wins;
  for (int numerator : numerators) wins[numerator] = 0;

  constexpr int kTrials = 200;
  constexpr int kBeats = 32;
  uint32_t state = 20260821u;
  const auto next_unit = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
  };

  for (int trial = 0; trial < kTrials; ++trial) {
    MeterBeats observations;
    observations.accent.reserve(kBeats);
    for (int beat = 0; beat < kBeats; ++beat) observations.accent.push_back(next_unit());
    ++wins[estimate_meter_from_observations(observations, config).time_signature.numerator];
  }

  const auto extremes = std::minmax_element(
      wins.begin(), wins.end(),
      [](const std::pair<const int, int>& lhs, const std::pair<const int, int>& rhs) {
        return lhs.second < rhs.second;
      });
  CAPTURE(wins[3], wins[4], wins[5], wins[7], wins[9], wins[11], wins[13]);
  REQUIRE(extremes.second->second <= kTrials / 2);
  REQUIRE(wins[13] * 3 >= wins[3]);
  REQUIRE(wins[3] * 3 >= wins[13]);
}

TEST_CASE("estimate_meter_from_observations reports odd numerators only when requested",
          "[meter_analyzer]") {
  for (int numerator : {5, 7}) {
    const MeterBeats observations = make_meter_beats(numerator, 5);
    MeterConfig widened;
    widened.candidate_numerators = {3, 4, 5, 6, 7};
    const MeterResult widened_result = estimate_meter_from_observations(observations, widened);

    CAPTURE(numerator, widened_result.candidate_scores);
    REQUIRE(widened_result.time_signature.numerator == numerator);
    REQUIRE(widened_result.time_signature.denominator == 4);
    REQUIRE(widened_result.time_signature.confidence > 0.5f);

    const MeterConfig defaults;
    const MeterResult default_result = estimate_meter_from_observations(observations, defaults);
    const int default_numerator = default_result.time_signature.numerator;
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != numerator);
    REQUIRE((default_numerator == 3 || default_numerator == 4 || default_numerator == 6));
  }
}

TEST_CASE("estimate_meter_from_observations reports the requested beat unit", "[meter_analyzer]") {
  const MeterBeats observations = make_meter_beats(4, 8);
  MeterConfig eighth;
  eighth.denominator = 8;
  const MeterResult result = estimate_meter_from_observations(observations, eighth);
  REQUIRE(result.time_signature.numerator == 4);
  REQUIRE(result.time_signature.denominator == 8);
  for (const TimeSignature& candidate : result.candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    REQUIRE(candidate.denominator == 8);
  }
}

TEST_CASE("estimate_meter_from_observations keeps the downbeat phase in range",
          "[meter_analyzer]") {
  std::vector<MeterConfig> configs(2);
  configs[1].candidate_numerators = {3, 4, 5, 6, 7};

  bool saw_non_zero_phase = false;
  for (int numerator : {3, 4, 5, 6, 7}) {
    const MeterBeats base = make_meter_beats(numerator, 6);
    for (int shift = 0; shift < numerator; ++shift) {
      MeterBeats rotated;
      rotated.accent.reserve(base.accent.size());
      for (size_t i = 0; i < base.accent.size(); ++i) {
        rotated.accent.push_back(base.accent[(i + base.accent.size() - static_cast<size_t>(shift)) %
                                             base.accent.size()]);
      }

      for (const MeterConfig& config : configs) {
        const MeterResult result = estimate_meter_from_observations(rotated, config);
        CAPTURE(numerator, shift, config.candidate_numerators, result.time_signature.numerator,
                result.downbeat_phase);
        REQUIRE(result.downbeat_phase >= 0);
        REQUIRE(result.downbeat_phase < result.time_signature.numerator);
        saw_non_zero_phase = saw_non_zero_phase || result.downbeat_phase > 0;
      }
    }
  }
  REQUIRE(saw_non_zero_phase);
}

TEST_CASE("estimate_meter_from_observations reports the accent grouping", "[meter_analyzer]") {
  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6, 7, 9, 11, 13};
  const std::vector<std::vector<int>> layouts = {
      {3, 2, 2}, {2, 3, 2}, {2, 2, 3}, {3, 2}, {2, 3}, {2, 2, 2, 3}, {3, 3, 3, 2, 2},
  };
  for (const std::vector<int>& grouping : layouts) {
    const MeterBeats observations = make_grouped_observations(grouping, 12);
    const MeterResult result = estimate_meter_from_observations(observations, config);
    CAPTURE(grouping, result.time_signature.numerator, result.grouping);
    REQUIRE(result.time_signature.numerator ==
            std::accumulate(grouping.begin(), grouping.end(), 0));
    REQUIRE(result.grouping == grouping);
  }
}

TEST_CASE("estimate_meter_from_observations groups every reported numerator", "[meter_analyzer]") {
  MeterConfig config;
  for (int numerator = kMinMeterCandidateNumerator; numerator <= kMaxMeterCandidateNumerator;
       ++numerator) {
    config.candidate_numerators = {numerator};
    for (const int measures : {1, 3, 12}) {
      const MeterBeats observations = make_meter_beats(numerator, measures);
      const MeterResult result = estimate_meter_from_observations(observations, config);
      CAPTURE(numerator, measures, result.time_signature.numerator, result.grouping);
      REQUIRE_FALSE(result.grouping.empty());
      REQUIRE(std::accumulate(result.grouping.begin(), result.grouping.end(), 0) ==
              result.time_signature.numerator);
      for (const int part : result.grouping) REQUIRE(part > 0);
    }
  }
}

TEST_CASE("estimate_meter_from_observations divides only searched numerators", "[meter_analyzer]") {
  MeterConfig config;
  bool saw_divided = false;
  for (int numerator = kMinMeterCandidateNumerator; numerator <= kMaxMeterCandidateNumerator;
       ++numerator) {
    config.candidate_numerators = {numerator};
    const MeterBeats observations = make_meter_beats(numerator, 12);
    const MeterResult result = estimate_meter_from_observations(observations, config);
    if (result.time_signature.numerator != numerator) continue;

    CAPTURE(numerator, result.grouping);
    if (numerator > kMaxGroupedMeterNumerator) {
      REQUIRE(result.grouping == std::vector<int>{numerator});
    } else {
      for (const int part : result.grouping) REQUIRE((part == 2 || part == 3));
      saw_divided = saw_divided || result.grouping.size() > 1;
    }
  }
  REQUIRE(saw_divided);
}

TEST_CASE("estimate_meter_from_observations keeps a six in its measured grouping",
          "[meter_analyzer]") {
  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};

  const MeterBeats compound = make_grouped_observations({3, 3}, 12);
  const MeterResult compound_result = estimate_meter_from_observations(compound, config);
  REQUIRE(compound_result.time_signature.numerator == 6);
  REQUIRE(compound_result.time_signature.denominator == config.denominator);
  REQUIRE(compound_result.grouping == std::vector<int>{3, 3});

  const MeterBeats simple = make_grouped_observations({2, 2, 2}, 12);
  const MeterResult simple_result = estimate_meter_from_observations(simple, config);
  REQUIRE(simple_result.time_signature.numerator == 6);
  REQUIRE(simple_result.time_signature.denominator == config.denominator);
  REQUIRE(simple_result.grouping == std::vector<int>{2, 2, 2});
}

TEST_CASE("meter grouping scores do not favour divisible numerators on noise", "[meter_analyzer]") {
  constexpr int kTrials = 200;
  constexpr int kBeats = 32;
  const std::vector<int> candidates = {4, 5, 7, 9, 11, 13};

  MeterConfig config;
  config.candidate_numerators = candidates;
  config.subdivision_weight = 1.0f;

  std::map<int, int> wins;
  for (int candidate : candidates) wins[candidate] = 0;
  uint32_t state = 20260821u;
  for (int trial = 0; trial < kTrials; ++trial) {
    MeterBeats observations;
    observations.accent.reserve(kBeats);
    for (int i = 0; i < kBeats; ++i) {
      state = state * 1664525u + 1013904223u;
      observations.accent.push_back(static_cast<float>(state >> 8) / 16777216.0f);
    }
    const MeterResult result = estimate_meter_from_observations(observations, config);
    const auto best =
        std::max_element(result.candidate_scores.begin(), result.candidate_scores.end());
    ++wins[candidates[static_cast<size_t>(std::distance(result.candidate_scores.begin(), best))]];
  }

  CAPTURE(wins);
  REQUIRE(wins[13] * 3 >= wins[4]);
  REQUIRE(wins[4] * 3 >= wins[13]);
  const auto extremes = std::minmax_element(
      wins.begin(), wins.end(),
      [](const std::pair<const int, int>& lhs, const std::pair<const int, int>& rhs) {
        return lhs.second < rhs.second;
      });
  REQUIRE(extremes.second->second <= kTrials / 2);
}

TEST_CASE("MeterAnalyzer leaves out a beat whose onset a louder event masked", "[meter_analyzer]") {
  // A downbeat with no onset but 30x the energy of every other beat.
  constexpr int kMasked = 9;
  auto beats = make_beats(3, 8);
  beats[kMasked].strength = 0.0f;
  const BeatAccents accents = make_accents(beats, 0.0f);
  std::vector<float> energy(beats.size(), 1.0f);
  energy[kMasked] = 30.0f;

  for (const bool with_accents : {true, false}) {
    const MeterResult result =
        estimate_meter(beats, MeterConfig(), with_accents ? accents : BeatAccents{}, energy);
    CAPTURE(with_accents, result.time_signature.numerator, result.unobserved_beats);
    REQUIRE(result.unobserved_beats == std::vector<int>{kMasked});
    REQUIRE(result.time_signature.numerator == 3);
  }
}

TEST_CASE("MeterAnalyzer keeps a rest as meter evidence", "[meter_analyzer]") {
  // Same silent beat, but quiet in energy too: that is a rest, not a masked beat.
  constexpr int kRest = 9;
  auto beats = make_beats(3, 8);
  beats[kRest].strength = 0.0f;
  const BeatAccents accents = make_accents(beats, 0.0f);
  std::vector<float> energy(beats.size(), 1.0f);
  energy[kRest] = 0.0f;

  const MeterResult with_energy = estimate_meter(beats, MeterConfig(), accents, energy);
  const MeterResult without_energy = estimate_meter(beats, MeterConfig(), accents);
  REQUIRE(with_energy.unobserved_beats.empty());
  REQUIRE(with_energy.candidate_scores == without_energy.candidate_scores);
  REQUIRE(with_energy.time_signature.numerator == without_energy.time_signature.numerator);
}

TEST_CASE("MeterAnalyzer without a dominant energy beat scores as without energy",
          "[meter_analyzer]") {
  const auto beats = make_beats(4, 6);
  const BeatAccents measured = make_accents(beats, 0.0f);
  const std::vector<float> flat_energy(beats.size(), 1.0f);

  for (const bool with_accents : {true, false}) {
    const BeatAccents accents = with_accents ? measured : BeatAccents{};
    const MeterResult base = estimate_meter(beats, MeterConfig(), accents);
    const MeterResult empty = estimate_meter(beats, MeterConfig(), accents, {});
    const MeterResult flat = estimate_meter(beats, MeterConfig(), accents, flat_energy);
    CAPTURE(with_accents);
    REQUIRE(base.unobserved_beats.empty());
    REQUIRE(empty.unobserved_beats.empty());
    REQUIRE(flat.unobserved_beats.empty());
    for (const MeterResult* other : {&empty, &flat}) {
      REQUIRE(other->candidate_scores == base.candidate_scores);
      REQUIRE(other->time_signature.numerator == base.time_signature.numerator);
      REQUIRE(other->time_signature.confidence == base.time_signature.confidence);
      REQUIRE(other->downbeat_phase == base.downbeat_phase);
    }
  }
}
