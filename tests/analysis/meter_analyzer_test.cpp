/// @file meter_analyzer_test.cpp
/// @brief Tests for multi-comb meter analyzer.

#include "analysis/meter_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
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

/// @brief An onset envelope carrying each beat plus energy between the beats.
/// @details The compound reading is resolved from how much energy sits at the
///          beat midpoints relative to the beats themselves, so @p
///          subdivision_ratio is what that score comes out as. A test that
///          wants a compound meter resolved has to supply one: without an
///          envelope there is no subdivision to measure and the estimator
///          reports the numerator with its grouping instead of promoting it.
std::vector<float> make_envelope(const std::vector<Beat>& beats, float subdivision_ratio) {
  std::vector<float> onsets(static_cast<size_t>(beats.back().frame) + 20, 0.0f);
  for (size_t i = 0; i < beats.size(); ++i) {
    onsets[static_cast<size_t>(beats[i].frame)] = beats[i].strength;
    if (i + 1 < beats.size()) {
      const int midpoint = (beats[i].frame + beats[i + 1].frame) / 2;
      onsets[static_cast<size_t>(midpoint)] = beats[i].strength * subdivision_ratio;
    }
  }
  return onsets;
}

/// @brief A beat series in the two parallel arrays estimate_meter_from_beats takes.
struct BeatSeries {
  std::vector<float> times;
  std::vector<float> strengths;
};

BeatSeries make_beat_series(int numerator, int measures) {
  BeatSeries series;
  for (const auto& beat : make_beats(numerator, measures)) {
    series.times.push_back(beat.time);
    series.strengths.push_back(beat.strength);
  }
  return series;
}

/// @brief A beat series whose bars divide exactly as @p grouping says.
/// @details The downbeat is the loudest beat, each following group starts on a
///          middling one, and the rest are quiet, which is the accent shape an
///          additive meter is notated for.
BeatSeries make_grouped_series(const std::vector<int>& grouping, int measures) {
  std::vector<int> accent_positions;
  int position = 0;
  for (size_t i = 0; i + 1 < grouping.size(); ++i) {
    position += grouping[i];
    accent_positions.push_back(position);
  }
  const int numerator = std::accumulate(grouping.begin(), grouping.end(), 0);

  BeatSeries series;
  for (int i = 0; i < numerator * measures; ++i) {
    const int pos = i % numerator;
    float strength = 0.35f;
    if (pos == 0) {
      strength = 1.0f;
    } else if (std::find(accent_positions.begin(), accent_positions.end(), pos) !=
               accent_positions.end()) {
      strength = 0.65f;
    }
    series.times.push_back(static_cast<float>(i) * 0.5f);
    series.strengths.push_back(strength);
  }
  return series;
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
  // The promotion to eighths is a claim about how a beat divides, so it needs
  // an envelope carrying that subdivision. The same beats without one report
  // the six with its 3+3 grouping and the requested unit.
  const auto beats = make_beats(6, 8);
  MeterAnalyzer analyzer(make_envelope(beats, 1.0f), beats);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.time_signature().confidence > 0.5f);

  MeterAnalyzer unmeasured({}, beats);
  REQUIRE(unmeasured.time_signature().numerator == 6);
  REQUIRE(unmeasured.time_signature().denominator == 4);
  REQUIRE(unmeasured.result().grouping == std::vector<int>{3, 3});
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
  MeterAnalyzer compound_analyzer(make_envelope(six_beats, 1.0f), six_beats, half);
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
  MeterAnalyzer analyzer(make_envelope(beats, 1.0f), beats, config);

  REQUIRE(analyzer.time_signature().numerator == 6);
  REQUIRE(analyzer.time_signature().denominator == 8);

  // Without the subdivision the exception does not apply and the requested
  // unit stands.
  MeterAnalyzer unmeasured({}, beats, config);
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

TEST_CASE("estimate_meter_from_beats applies the config guards before scoring",
          "[meter_analyzer]") {
  const BeatSeries series = make_beat_series(4, 8);

  MeterConfig empty_candidates;
  empty_candidates.candidate_numerators.clear();
  REQUIRE_THROWS_AS(estimate_meter_from_beats(series.times, series.strengths, empty_candidates),
                    SonareException);

  MeterConfig odd_denominator;
  odd_denominator.denominator = 3;
  REQUIRE_THROWS_AS(estimate_meter_from_beats(series.times, series.strengths, odd_denominator),
                    SonareException);

  MeterConfig negative_weight;
  negative_weight.measure_weight = -1.0f;
  REQUIRE_THROWS_AS(estimate_meter_from_beats(series.times, series.strengths, negative_weight),
                    SonareException);
}

TEST_CASE("estimate_meter_from_beats rejects an unusable beat series", "[meter_analyzer]") {
  const BeatSeries series = make_beat_series(4, 8);

  // Mismatched lengths mean the caller paired the wrong two arrays; scoring the
  // shorter prefix would answer a question nobody asked.
  const std::vector<float> short_strengths(series.strengths.begin(), series.strengths.end() - 1);
  REQUIRE_THROWS_AS(estimate_meter_from_beats(series.times, short_strengths), SonareException);
  const std::vector<float> short_times(series.times.begin(), series.times.end() - 1);
  REQUIRE_THROWS_AS(estimate_meter_from_beats(short_times, series.strengths), SonareException);

  const auto rejects_times = [&series](void (*mutate)(std::vector<float>&)) {
    std::vector<float> times = series.times;
    mutate(times);
    REQUIRE_THROWS_AS(estimate_meter_from_beats(times, series.strengths), SonareException);
  };
  const auto rejects_strengths = [&series](void (*mutate)(std::vector<float>&)) {
    std::vector<float> strengths = series.strengths;
    mutate(strengths);
    REQUIRE_THROWS_AS(estimate_meter_from_beats(series.times, strengths), SonareException);
  };

  rejects_times([](std::vector<float>& t) { t[5] = t[4] - 0.25f; });
  rejects_times([](std::vector<float>& t) { t[3] = -0.5f; });
  rejects_times([](std::vector<float>& t) { t[3] = std::nanf(""); });
  rejects_times([](std::vector<float>& t) { t[3] = std::numeric_limits<float>::infinity(); });
  rejects_strengths([](std::vector<float>& s) { s[2] = std::nanf(""); });
  rejects_strengths([](std::vector<float>& s) { s[2] = std::numeric_limits<float>::infinity(); });
  rejects_strengths([](std::vector<float>& s) { s[2] = -std::numeric_limits<float>::infinity(); });

  // The guards stop where the contract does: a repeated beat time is still
  // non-decreasing, and a negative strength is still finite.
  std::vector<float> repeated = series.times;
  repeated[5] = repeated[4];
  REQUIRE_NOTHROW(estimate_meter_from_beats(repeated, series.strengths));
  std::vector<float> negative = series.strengths;
  negative[2] = -1.0f;
  REQUIRE_NOTHROW(estimate_meter_from_beats(series.times, negative));

  // No beats is not a short search but nothing to search, so it is rejected
  // rather than answered with the fixed low-confidence default.
  REQUIRE_THROWS_AS(estimate_meter_from_beats({}, {}), SonareException);
}

TEST_CASE("estimate_meter_from_beats draws the line at empty, not at short", "[meter_analyzer]") {
  const BeatSeries series = make_beat_series(4, 8);

  // Nothing to score is rejected: answering it would report a 4/4 the caller
  // supplied no evidence for, the same objection validate_meter_config makes to
  // an empty candidate list and through the same code path.
  const std::vector<float> empty;
  REQUIRE_THROWS_AS(estimate_meter_from_beats(empty, empty), SonareException);
  REQUIRE_THROWS_AS(estimate_meter_from_beats({}, {}), SonareException);

  // One beat is a series, so it is answered — with the low-confidence default
  // rather than a search result. Without this the rejection above would be
  // equally consistent with a guard that swallowed every short series, which is
  // not where the line is drawn.
  const std::vector<float> one_time(series.times.begin(), series.times.begin() + 1);
  const std::vector<float> one_strength(series.strengths.begin(), series.strengths.begin() + 1);
  MeterResult one;
  REQUIRE_NOTHROW(one = estimate_meter_from_beats(one_time, one_strength));
  REQUIRE(one.time_signature.numerator == 4);
  REQUIRE(one.time_signature.denominator == 4);
  REQUIRE(one.downbeat_phase == 0);
  REQUIRE(one.candidates.size() == 1);
  REQUIRE(one.time_signature.confidence <= 0.5f);

  // A full series scores higher, so the value above is the short-series report
  // and not the estimator's ceiling.
  const MeterResult searched = estimate_meter_from_beats(series.times, series.strengths);
  REQUIRE(searched.time_signature.confidence > one.time_signature.confidence);
}

TEST_CASE("estimate_meter_from_beats reports the default for a series below the search floor",
          "[meter_analyzer]") {
  // Fewer than eight beats gives the comb nothing to score, so the answer is
  // the documented default rather than a detection. What separates the two is
  // the searched flag, which says so outright instead of leaving a caller to
  // recognize the fallback by its confidence.
  const BeatSeries full = make_beat_series(4, 8);
  const MeterResult searched = estimate_meter_from_beats(full.times, full.strengths);
  REQUIRE(searched.searched);
  REQUIRE(searched.time_signature.confidence > 0.5f);

  for (int count = 1; count < 8; ++count) {
    const auto end = static_cast<std::ptrdiff_t>(count);
    const std::vector<float> times(full.times.begin(), full.times.begin() + end);
    const std::vector<float> strengths(full.strengths.begin(), full.strengths.begin() + end);
    const MeterResult result = estimate_meter_from_beats(times, strengths);

    CAPTURE(count, result.time_signature.numerator, result.time_signature.confidence);
    REQUIRE_FALSE(result.searched);
    REQUIRE(result.time_signature.numerator == 4);
    REQUIRE(result.time_signature.denominator == 4);
    REQUIRE(result.downbeat_phase == 0);
    REQUIRE(result.candidates.size() == 1);
    // Every score belongs to the fallback rather than to a candidate, so none
    // of them ranks anything.
    REQUIRE(std::all_of(result.candidate_scores.begin(), result.candidate_scores.end(),
                        [](float score) { return score == 0.0f; }));
    // The short-span answer has to be distinguishable from a real detection.
    REQUIRE(result.time_signature.confidence < searched.time_signature.confidence);
    REQUIRE(result.time_signature.confidence <= 0.5f);
  }
}

TEST_CASE("estimate_meter_from_beats cannot resolve a compound meter from accents alone",
          "[meter_analyzer]") {
  // Whether a beat divides into three is answered from energy between the
  // beats, which a per-beat accent series does not carry. A six accented 3+3
  // therefore keeps the beat unit the caller asked for and says how its bar
  // divides through the grouping, rather than being promoted to a 6/8 that
  // nothing measured.
  std::vector<float> times;
  std::vector<float> strengths;
  for (int bar = 0; bar < 16; ++bar) {
    for (int position = 0; position < 6; ++position) {
      times.push_back(0.5f * static_cast<float>(bar * 6 + position));
      strengths.push_back(position == 0 ? 1.0f : (position == 3 ? 0.7f : 0.3f));
    }
  }

  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};
  const MeterResult result = estimate_meter_from_beats(times, strengths, config);

  REQUIRE(result.searched);
  REQUIRE(result.time_signature.numerator == 6);
  REQUIRE(result.time_signature.denominator == 4);
  REQUIRE(result.grouping == std::vector<int>{3, 3});
  // Every listed six agrees with the primary result about the beat unit.
  for (const TimeSignature& candidate : result.candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    if (candidate.numerator == 6) REQUIRE(candidate.denominator == 4);
  }

  // A caller who asks for eighths gets eighths, and still gets the grouping
  // rather than an inferred compound reading.
  MeterConfig eighths = config;
  eighths.denominator = 8;
  const MeterResult in_eighths = estimate_meter_from_beats(times, strengths, eighths);
  REQUIRE(in_eighths.time_signature.numerator == 6);
  REQUIRE(in_eighths.time_signature.denominator == 8);
  REQUIRE(in_eighths.grouping == std::vector<int>{3, 3});

  // A threshold of zero would make an unmeasured subdivision compare as
  // satisfied against a score that is zero because nothing was measured.
  MeterConfig permissive = config;
  permissive.compound_subdivision_threshold = 0.0f;
  const MeterResult permissive_result = estimate_meter_from_beats(times, strengths, permissive);
  REQUIRE(permissive_result.time_signature.denominator == 4);
}

TEST_CASE("estimate_meter_from_beats scores are not comparable across span lengths",
          "[meter_analyzer]") {
  // The documented reason a segmentation search cannot rank spans by raw score:
  // evidence for a repeating accent accumulates with the number of beats
  // carrying it, so the same meter over more beats scores higher. Pinning the
  // growth keeps the docs honest about what the number is.
  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};

  const auto top_score = [&config](int bars) {
    const BeatSeries series = make_beat_series(4, bars);
    const MeterResult result = estimate_meter_from_beats(series.times, series.strengths, config);
    REQUIRE(result.searched);
    REQUIRE(result.time_signature.numerator == 4);
    return *std::max_element(result.candidate_scores.begin(), result.candidate_scores.end());
  };

  const float short_span = top_score(4);
  const float long_span = top_score(16);
  CAPTURE(short_span, long_span);
  // Four times the beats, so about twice the score: same meter, same accents,
  // a score that ranks by length as much as by meter.
  REQUIRE(long_span > short_span * 1.5f);
  REQUIRE(long_span < short_span * 2.5f);
}

TEST_CASE("estimate_meter_from_beats detects 4/4 from a caller-supplied series",
          "[meter_analyzer]") {
  const BeatSeries series = make_beat_series(4, 8);
  const MeterResult result = estimate_meter_from_beats(series.times, series.strengths);

  REQUIRE(result.time_signature.numerator == 4);
  REQUIRE(result.time_signature.denominator == 4);
  REQUIRE(result.time_signature.confidence > 0.5f);
  REQUIRE(result.downbeat_phase == 0);
  REQUIRE(result.candidate_scores.size() == MeterConfig().candidate_numerators.size());
  REQUIRE_FALSE(result.candidates.empty());
  REQUIRE(result.candidates.front().numerator == 4);
  REQUIRE(result.candidates.front().denominator == 4);

  // This entry point differs from estimate_meter only in how the beats are
  // packed, and the frame index it synthesizes goes unread while the onset
  // envelope is empty, so the two results must agree exactly.
  const MeterResult direct = estimate_meter({}, make_beats(4, 8));
  REQUIRE(result.time_signature.numerator == direct.time_signature.numerator);
  REQUIRE(result.time_signature.denominator == direct.time_signature.denominator);
  REQUIRE(result.time_signature.confidence == direct.time_signature.confidence);
  REQUIRE(result.downbeat_phase == direct.downbeat_phase);
  REQUIRE(result.candidate_scores == direct.candidate_scores);
  REQUIRE(result.candidates.size() == direct.candidates.size());
}

TEST_CASE("estimate_meter_from_beats reads accent contrast, not absolute level",
          "[meter_analyzer]") {
  const BeatSeries unit = make_beat_series(4, 8);

  // The stream this entry point documents as its input --
  // AnalysisResult::beat_observations.onset_strength -- is a windowed aggregate
  // of the onset envelope in the envelope's own units, so every value in it
  // routinely exceeds 1 on ordinary material. Scoring must divide the series by
  // its own maximum; clamping it instead flattens every beat to the same value
  // and leaves the candidates tied, which reads as a detection of whichever one
  // was listed first.
  BeatSeries scaled = unit;
  for (float& strength : scaled.strengths) {
    strength *= 20.0f;
  }

  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6, 7};
  const MeterResult unit_result = estimate_meter_from_beats(unit.times, unit.strengths, config);
  const MeterResult scaled_result =
      estimate_meter_from_beats(scaled.times, scaled.strengths, config);

  REQUIRE(scaled_result.time_signature.numerator == 4);
  REQUIRE(scaled_result.time_signature.numerator == unit_result.time_signature.numerator);
  REQUIRE(scaled_result.time_signature.denominator == unit_result.time_signature.denominator);
  REQUIRE(scaled_result.time_signature.confidence == unit_result.time_signature.confidence);
  REQUIRE(scaled_result.downbeat_phase == unit_result.downbeat_phase);
  REQUIRE(scaled_result.candidate_scores == unit_result.candidate_scores);

  // A tie across every candidate is what saturation produces, so name it: the
  // winning candidate has to stand above the rest on its own score.
  const auto& scores = scaled_result.candidate_scores;
  const size_t best = static_cast<size_t>(
      std::distance(scores.begin(), std::max_element(scores.begin(), scores.end())));
  REQUIRE(config.candidate_numerators[best] == 4);
  for (size_t i = 0; i < scores.size(); ++i) {
    if (i != best) {
      REQUIRE(scores[i] < scores[best]);
    }
  }
}

TEST_CASE("estimate_meter_from_beats does not favour a wide numerator on unmetred beats",
          "[meter_analyzer]") {
  // A candidate keeps the best of its own phases, so a wide numerator has more
  // phases to win that maximum from noise and fewer beats on each downbeat to
  // average it over. Left uncorrected both push the same way, and on beats
  // carrying no meter at all the widest candidate wins several times as often as
  // the narrowest -- which makes a wide candidate list unusable on material
  // whose accents are only mildly contrasted, as a real onset stream's are.
  // Scoring unmetred series is the direct measurement of that, so it is what
  // this asserts. The series are deterministic, so the counts below are fixed.
  const std::vector<int> numerators = {3, 4, 5, 7, 9, 11, 13};
  MeterConfig config;
  config.candidate_numerators = numerators;

  std::map<int, int> wins;
  for (int numerator : numerators) {
    wins[numerator] = 0;
  }

  constexpr int kTrials = 200;
  constexpr int kBeats = 32;
  uint32_t state = 20260821u;
  const auto next_unit = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
  };

  for (int trial = 0; trial < kTrials; ++trial) {
    std::vector<float> times;
    std::vector<float> strengths;
    for (int beat = 0; beat < kBeats; ++beat) {
      times.push_back(static_cast<float>(beat) * 0.5f);
      strengths.push_back(next_unit());
    }
    ++wins[estimate_meter_from_beats(times, strengths, config).time_signature.numerator];
  }

  const auto extremes = std::minmax_element(
      wins.begin(), wins.end(),
      [](const std::pair<const int, int>& lhs, const std::pair<const int, int>& rhs) {
        return lhs.second < rhs.second;
      });
  CAPTURE(wins[3], wins[4], wins[5], wins[7], wins[9], wins[11], wins[13]);

  // An even split is 200 / 7 ~= 29 each. The bound is loose enough that ordinary
  // sampling spread cannot trip it and tight enough that the bias it guards
  // against -- which took more than half the trials for the widest candidate --
  // cannot pass.
  REQUIRE(extremes.second->second <= kTrials / 2);
  REQUIRE(wins[13] * 3 >= wins[3]);
  REQUIRE(wins[3] * 3 >= wins[13]);
}

TEST_CASE("estimate_meter_from_beats reports an odd meter only when it is a candidate",
          "[meter_analyzer]") {
  for (int numerator : {5, 7}) {
    const BeatSeries series = make_beat_series(numerator, 5);

    MeterConfig widened;
    widened.candidate_numerators = {3, 4, 5, 6, 7};
    const MeterResult widened_result =
        estimate_meter_from_beats(series.times, series.strengths, widened);

    CAPTURE(numerator, widened_result.candidate_scores);
    REQUIRE(widened_result.time_signature.numerator == numerator);
    REQUIRE(widened_result.time_signature.denominator == 4);
    REQUIRE(widened_result.time_signature.confidence > 0.5f);

    // The same series under the default candidate set cannot reach that
    // numerator, so the detection above came from the config rather than from
    // the accent pattern alone.
    const MeterResult default_result = estimate_meter_from_beats(series.times, series.strengths);
    const int default_numerator = default_result.time_signature.numerator;
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != numerator);
    REQUIRE((default_numerator == 3 || default_numerator == 4 || default_numerator == 6));
  }
}

TEST_CASE("estimate_meter_from_beats reports the requested beat unit", "[meter_analyzer]") {
  // A 4-beat accent pattern never reaches the compound branch, the one place
  // the estimator overrides the requested unit.
  const BeatSeries series = make_beat_series(4, 8);

  MeterConfig eighth;
  eighth.denominator = 8;
  const MeterResult eighth_result =
      estimate_meter_from_beats(series.times, series.strengths, eighth);
  REQUIRE(eighth_result.time_signature.numerator == 4);
  REQUIRE(eighth_result.time_signature.denominator == 8);
  for (const auto& candidate : eighth_result.candidates) {
    CAPTURE(candidate.numerator, candidate.denominator);
    REQUIRE(candidate.denominator == 8);
  }
}

TEST_CASE("estimate_meter_from_beats keeps the downbeat phase inside the numerator",
          "[meter_analyzer]") {
  std::vector<MeterConfig> configs(2);
  configs[1].candidate_numerators = {3, 4, 5, 6, 7};

  bool saw_non_zero_phase = false;
  for (int numerator : {3, 4, 5, 6, 7}) {
    const BeatSeries base = make_beat_series(numerator, 6);
    const size_t count = base.strengths.size();
    for (int shift = 0; shift < numerator; ++shift) {
      // Rotating the accents moves the true measure start off beat 0, so the
      // best-scoring phase is non-zero for at least some of these.
      BeatSeries rotated;
      rotated.times = base.times;
      for (size_t i = 0; i < count; ++i) {
        rotated.strengths.push_back(
            base.strengths[(i + count - static_cast<size_t>(shift)) % count]);
      }

      for (const MeterConfig& config : configs) {
        const MeterResult result =
            estimate_meter_from_beats(rotated.times, rotated.strengths, config);
        CAPTURE(numerator, shift, config.candidate_numerators, result.time_signature.numerator,
                result.downbeat_phase);
        REQUIRE(result.downbeat_phase >= 0);
        REQUIRE(result.downbeat_phase < result.time_signature.numerator);
        saw_non_zero_phase = saw_non_zero_phase || result.downbeat_phase > 0;
      }
    }
  }

  // Without a non-zero phase somewhere the range check above would hold for a
  // constant-zero implementation.
  REQUIRE(saw_non_zero_phase);
}

TEST_CASE("estimate_meter_from_beats reports how the bar divides", "[meter_analyzer]") {
  // Two sevens carrying the same numerator and the same number of accents, laid
  // out differently. Reporting the numerator alone cannot tell them apart, which
  // is the whole point of the grouping.
  MeterConfig config;
  config.candidate_numerators = {3, 4, 5, 6, 7, 9, 11, 13};

  const std::vector<std::vector<int>> layouts = {
      {3, 2, 2}, {2, 3, 2}, {2, 2, 3}, {3, 2}, {2, 3}, {2, 2, 2, 3}, {3, 3, 3, 2, 2},
  };
  for (const std::vector<int>& grouping : layouts) {
    const BeatSeries series = make_grouped_series(grouping, 12);
    const MeterResult result = estimate_meter_from_beats(series.times, series.strengths, config);

    CAPTURE(grouping, result.time_signature.numerator, result.grouping);
    REQUIRE(result.time_signature.numerator ==
            std::accumulate(grouping.begin(), grouping.end(), 0));
    REQUIRE(result.grouping == grouping);
  }
}

TEST_CASE("estimate_meter_from_beats always reports a grouping that sums to the numerator",
          "[meter_analyzer]") {
  // The invariant every consumer of the field relies on, checked over the whole
  // candidate range including the numerators too wide to search and the spans
  // too short to search at all.
  MeterConfig config;
  for (int numerator = kMinMeterCandidateNumerator; numerator <= kMaxMeterCandidateNumerator;
       ++numerator) {
    config.candidate_numerators = {numerator};
    for (const int measures : {1, 3, 12}) {
      const BeatSeries series = make_beat_series(numerator, measures);
      const MeterResult result = estimate_meter_from_beats(series.times, series.strengths, config);

      CAPTURE(numerator, measures, result.time_signature.numerator, result.grouping);
      REQUIRE_FALSE(result.grouping.empty());
      REQUIRE(std::accumulate(result.grouping.begin(), result.grouping.end(), 0) ==
              result.time_signature.numerator);
      for (const int part : result.grouping) {
        REQUIRE(part > 0);
      }
    }
  }
}

TEST_CASE("estimate_meter_from_beats divides only the numerators it can search",
          "[meter_analyzer]") {
  MeterConfig config;
  bool saw_divided = false;
  for (int numerator = kMinMeterCandidateNumerator; numerator <= kMaxMeterCandidateNumerator;
       ++numerator) {
    config.candidate_numerators = {numerator};
    const BeatSeries series = make_beat_series(numerator, 12);
    const MeterResult result = estimate_meter_from_beats(series.times, series.strengths, config);
    if (result.time_signature.numerator != numerator) continue;

    CAPTURE(numerator, result.grouping);
    if (numerator > kMaxGroupedMeterNumerator) {
      // Too wide to search: one undivided group rather than a guess.
      REQUIRE(result.grouping == std::vector<int>{numerator});
    } else {
      // Inside the searched range every part is a two or a three, and 2 and 3
      // are the two numerators that legitimately come back undivided.
      for (const int part : result.grouping) {
        REQUIRE((part == 2 || part == 3));
      }
      saw_divided = saw_divided || result.grouping.size() > 1;
    }
  }
  REQUIRE(saw_divided);
}

TEST_CASE("estimate_meter_from_beats leaves a six undivided until its beats divide it",
          "[meter_analyzer]") {
  // A six that groups into two threes reports that grouping; a six that groups
  // into three twos reports its own -- it used to be pushed through the
  // compound branch and come back out as a three or a four. Neither changes the
  // beat unit here: this entry point has no subdivision to measure, so the
  // grouping is the whole of what separates the two readings.
  MeterConfig config;
  config.candidate_numerators = {3, 4, 6};

  const BeatSeries compound = make_grouped_series({3, 3}, 12);
  const MeterResult compound_result =
      estimate_meter_from_beats(compound.times, compound.strengths, config);
  REQUIRE(compound_result.time_signature.numerator == 6);
  REQUIRE(compound_result.time_signature.denominator == config.denominator);
  REQUIRE(compound_result.grouping == std::vector<int>{3, 3});

  const BeatSeries simple = make_grouped_series({2, 2, 2}, 12);
  const MeterResult simple_result =
      estimate_meter_from_beats(simple.times, simple.strengths, config);
  REQUIRE(simple_result.time_signature.numerator == 6);
  REQUIRE(simple_result.time_signature.denominator == config.denominator);
  REQUIRE(simple_result.grouping == std::vector<int>{2, 2, 2});
}

TEST_CASE("estimate_meter_from_beats does not favour a divisible numerator on unmetred beats",
          "[meter_analyzer]") {
  // Picking the best of a numerator's groupings is a second search on top of the
  // phase search, and a maximum over more alternatives is worth more on noise
  // alone. Numerators divide into wildly different numbers of groupings -- 4 has
  // one, 13 has sixteen -- so an uncorrected grouping search hands the divisible
  // numerators a lead that has nothing to do with the beats.
  constexpr int kTrials = 200;
  constexpr int kBeats = 32;
  const std::vector<int> candidates = {4, 5, 7, 9, 11, 13};

  MeterConfig config;
  config.candidate_numerators = candidates;
  // At the default 0.15 the grouping term is a small part of the score and its
  // bias hides inside the phase search's own noise at any trial count a test can
  // afford. Weighting it like the downbeat brings the effect into view, which is
  // what makes the thresholds below able to fail.
  config.subdivision_weight = 1.0f;

  std::vector<float> times(kBeats);
  for (int i = 0; i < kBeats; ++i) {
    times[static_cast<size_t>(i)] = static_cast<float>(i) * 0.5f;
  }

  std::map<int, int> wins;
  for (int candidate : candidates) {
    wins[candidate] = 0;
  }
  uint32_t state = 20260821u;
  for (int trial = 0; trial < kTrials; ++trial) {
    std::vector<float> strengths(kBeats);
    for (int i = 0; i < kBeats; ++i) {
      state = state * 1664525u + 1013904223u;
      strengths[static_cast<size_t>(i)] = static_cast<float>(state >> 8) / 16777216.0f;
    }
    const MeterResult result = estimate_meter_from_beats(times, strengths, config);
    const auto best =
        std::max_element(result.candidate_scores.begin(), result.candidate_scores.end());
    ++wins[candidates[static_cast<size_t>(std::distance(result.candidate_scores.begin(), best))]];
  }

  CAPTURE(wins);
  // Thirteen divides sixteen ways and four divides one way, so if the grouping
  // search went uncorrected this is where it would show.
  REQUIRE(wins[13] * 3 >= wins[4]);
  REQUIRE(wins[4] * 3 >= wins[13]);
  const auto extremes = std::minmax_element(
      wins.begin(), wins.end(),
      [](const std::pair<const int, int>& lhs, const std::pair<const int, int>& rhs) {
        return lhs.second < rhs.second;
      });
  REQUIRE(extremes.second->second <= kTrials / 2);
}
