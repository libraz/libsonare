/// @file meter_analyzer_test.cpp
/// @brief Tests for multi-comb meter analyzer.

#include "analysis/meter_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
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
  // the confidence, which is why that value is the contract here.
  const BeatSeries full = make_beat_series(4, 8);
  const MeterResult searched = estimate_meter_from_beats(full.times, full.strengths);
  REQUIRE(searched.time_signature.confidence > 0.5f);

  for (int count = 1; count < 8; ++count) {
    const auto end = static_cast<std::ptrdiff_t>(count);
    const std::vector<float> times(full.times.begin(), full.times.begin() + end);
    const std::vector<float> strengths(full.strengths.begin(), full.strengths.begin() + end);
    const MeterResult result = estimate_meter_from_beats(times, strengths);

    CAPTURE(count, result.time_signature.numerator, result.time_signature.confidence);
    REQUIRE(result.time_signature.numerator == 4);
    REQUIRE(result.time_signature.denominator == 4);
    REQUIRE(result.downbeat_phase == 0);
    REQUIRE(result.candidates.size() == 1);
    // The short-span answer has to be distinguishable from a real detection.
    REQUIRE(result.time_signature.confidence < searched.time_signature.confidence);
    REQUIRE(result.time_signature.confidence <= 0.5f);
  }
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
