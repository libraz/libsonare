/// @file db_convert_test.cpp
/// @brief Reference compatibility tests for standalone dB conversions.

#include "core/db_convert.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("power_to_db scalar matches librosa", "[librosa][db_convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/db_conversion.json");
  const auto& cases = json["data"]["power_to_db_scalar_no_topdb"].as_array();
  for (size_t i = 0; i < cases.size(); ++i) {
    float power = cases[i]["power"].as_float();
    float expected = cases[i]["db"].as_float();
    std::vector<float> input{power};
    auto got = power_to_db(input, 1.0f, 1e-10f, -1.0f);
    CAPTURE(i, power, got[0], expected);
    REQUIRE_THAT(got[0], WithinAbs(expected, 1e-4f));
  }
}

TEST_CASE("amplitude_to_db scalar matches librosa", "[librosa][db_convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/db_conversion.json");
  const auto& cases = json["data"]["amplitude_to_db_scalar"].as_array();
  for (size_t i = 0; i < cases.size(); ++i) {
    float amp = cases[i]["amplitude"].as_float();
    float expected = cases[i]["db"].as_float();
    std::vector<float> input{amp};
    auto got = amplitude_to_db(input, 1.0f, 1e-5f, -1.0f);
    CAPTURE(i, amp, got[0], expected);
    REQUIRE_THAT(got[0], WithinAbs(expected, 1e-3f));
  }
}

TEST_CASE("db_to_power inverse matches librosa", "[librosa][db_convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/db_conversion.json");
  const auto& cases = json["data"]["db_to_power"].as_array();
  for (size_t i = 0; i < cases.size(); ++i) {
    float db = cases[i]["db"].as_float();
    float expected = cases[i]["power"].as_float();
    std::vector<float> input{db};
    auto got = db_to_power(input);
    CAPTURE(i, db, got[0], expected);
    REQUIRE_THAT(got[0], WithinRel(expected, 1e-5f));
  }
}

TEST_CASE("db_to_amplitude inverse matches librosa", "[librosa][db_convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/db_conversion.json");
  const auto& cases = json["data"]["db_to_amplitude"].as_array();
  for (size_t i = 0; i < cases.size(); ++i) {
    float db = cases[i]["db"].as_float();
    float expected = cases[i]["amplitude"].as_float();
    std::vector<float> input{db};
    auto got = db_to_amplitude(input);
    CAPTURE(i, db, got[0], expected);
    REQUIRE_THAT(got[0], WithinRel(expected, 1e-5f));
  }
}

TEST_CASE("power_to_db with ref=max and top_db matches librosa", "[librosa][db_convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/db_conversion.json");
  const auto& obj = json["data"]["power_to_db_maxref"];
  const auto& input = obj["data_flat"].as_array();
  const auto& expected = obj["expected_flat"].as_array();
  float amin = obj["amin"].as_float();
  float top_db = obj["top_db"].as_float();

  std::vector<float> S;
  S.reserve(input.size());
  for (const auto& v : input) S.push_back(v.as_float());

  // ref < 0 signals "use max(|S|)" in our API.
  auto got = power_to_db(S, -1.0f, amin, top_db);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    float e = expected[i].as_float();
    CAPTURE(i, got[i], e);
    REQUIRE_THAT(got[i], WithinAbs(e, 1e-3f));
  }
}

TEST_CASE("dB conversions reject invalid input", "[db_convert][edge]") {
  std::vector<float> empty;
  REQUIRE(power_to_db(empty).empty());
  REQUIRE(amplitude_to_db(empty).empty());
  REQUIRE(db_to_power(empty).empty());
  REQUIRE(db_to_amplitude(empty).empty());
  REQUIRE_THROWS_AS(power_to_db(nullptr, 5), SonareException);
  std::vector<float> v{0.5f};
  REQUIRE_THROWS_AS(power_to_db(v, 1.0f, 0.0f, 80.0f), SonareException);
}

TEST_CASE("dB conversions use the shared default top dB", "[db_convert]") {
  const std::vector<float> input{1.0f, 1e-20f};

  const auto default_power = power_to_db(input);
  const auto explicit_power =
      power_to_db(input, 1.0f, constants::kEpsilon, constants::kDefaultTopDb);
  const auto default_amplitude = amplitude_to_db(input);
  const auto explicit_amplitude = amplitude_to_db(input, 1.0f, 1e-5f, constants::kDefaultTopDb);

  REQUIRE(default_power == explicit_power);
  REQUIRE(default_amplitude == explicit_amplitude);
  REQUIRE_THAT(default_power[1], WithinAbs(-constants::kDefaultTopDb, 1e-5f));
}

TEST_CASE("dB conversions reject non-finite scalar parameters", "[db_convert][edge]") {
  const std::vector<float> input{1.0f};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  REQUIRE_THROWS_AS(power_to_db(input, nan), SonareException);
  REQUIRE_THROWS_AS(power_to_db(input, 1.0f, nan), SonareException);
  REQUIRE_THROWS_AS(power_to_db(input, 1.0f, 1e-10f, inf), SonareException);
  REQUIRE_THROWS_AS(amplitude_to_db(input, inf), SonareException);
  REQUIRE_THROWS_AS(amplitude_to_db(input, 1.0f, nan), SonareException);
  REQUIRE_THROWS_AS(amplitude_to_db(input, 1.0f, 1e-5f, nan), SonareException);
  REQUIRE_THROWS_AS(db_to_power(input, nan), SonareException);
  REQUIRE_THROWS_AS(db_to_amplitude(input, inf), SonareException);
}

// A NaN bin must reach the caller as NaN: librosa 0.11.0 returns nan here
// (np.maximum propagates, unlike np.fmax), and a floored NaN is indistinguishable
// from a genuinely silent bin. The finite and infinite bins are the control --
// the argument order in std::max moves nothing else.
TEST_CASE("power_to_db propagates a non-finite bin", "[librosa][db_convert][edge]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SECTION("a NaN bin comes out NaN and leaves its neighbours untouched") {
    const std::vector<float> S{1.0f, nan, 1e-12f, 0.25f};
    const auto got = power_to_db(S, 1.0f, 1e-10f, -1.0f);

    REQUIRE(got.size() == 4);
    REQUIRE(std::isnan(got[1]));
    // librosa.power_to_db([1, nan, 1e-12, 0.25], ref=1.0, amin=1e-10, top_db=None)
    // -> [0.0, nan, -100.0, -6.0206003]
    REQUIRE_THAT(got[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(got[2], WithinAbs(-100.0f, 1e-4f));
    REQUIRE_THAT(got[3], WithinAbs(-6.0206003f, 1e-4f));

    // The same bins without the NaN present are bit-identical, so the NaN does
    // not perturb the accumulation it passes through.
    const std::vector<float> finite_only{1.0f, 1e-12f, 0.25f};
    const auto reference = power_to_db(finite_only, 1.0f, 1e-10f, -1.0f);
    REQUIRE(got[0] == reference[0]);
    REQUIRE(got[2] == reference[1]);
    REQUIRE(got[3] == reference[2]);
  }

  SECTION("an infinite bin is unchanged: +inf stays +inf, -inf floors at amin") {
    const std::vector<float> S{1.0f, inf, -inf, 0.25f};
    const auto got = power_to_db(S, 1.0f, 1e-10f, -1.0f);

    REQUIRE(std::isinf(got[1]));
    REQUIRE(got[1] > 0.0f);
    REQUIRE_THAT(got[2], WithinAbs(-100.0f, 1e-4f));
    REQUIRE_THAT(got[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(got[3], WithinAbs(-6.0206003f, 1e-4f));
  }

  SECTION("top_db leaves a NaN alone in both passes") {
    // Neither `db > max_db` nor `out[i] < floor_db` is true for a NaN, so it is
    // excluded from the maximum and skipped by the clamp. librosa instead takes
    // a NaN maximum and returns nan for every bin; only the NaN bin agrees.
    const std::vector<float> S{1.0f, nan, 1e-12f, 0.25f};
    const auto got = power_to_db(S, 1.0f, 1e-10f, 80.0f);

    REQUIRE(std::isnan(got[1]));
    REQUIRE_THAT(got[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(got[2], WithinAbs(-80.0f, 1e-4f));
    REQUIRE_THAT(got[3], WithinAbs(-6.0206003f, 1e-4f));
  }

  SECTION("the max-reference sentinel keeps a finite reference") {
    // resolve_ref folds with the accumulator first, so it absorbs the NaN and the
    // reference stays the finite maximum. librosa's ref=np.max is NaN instead and
    // returns nan for every bin; the divergence is the reference, not this bin.
    const std::vector<float> S{1.0f, nan, 1e-12f, 0.25f};
    const auto got = power_to_db(S, -1.0f, 1e-10f, -1.0f);

    REQUIRE(std::isnan(got[1]));
    REQUIRE_THAT(got[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(got[2], WithinAbs(-100.0f, 1e-4f));
    REQUIRE_THAT(got[3], WithinAbs(-6.0206003f, 1e-4f));
  }

  SECTION("amplitude_to_db inherits the propagation through the squaring") {
    const std::vector<float> A{1.0f, nan, 1e-6f, 0.5f};
    const auto got = amplitude_to_db(A, 1.0f, 1e-5f, -1.0f);

    REQUIRE(std::isnan(got[1]));
    // librosa.amplitude_to_db([1, nan, 1e-6, 0.5], ref=1.0, amin=1e-5, top_db=None)
    // -> [0.0, nan, -100.0, -6.0206003]
    REQUIRE_THAT(got[0], WithinAbs(0.0f, 1e-3f));
    REQUIRE_THAT(got[2], WithinAbs(-100.0f, 1e-3f));
    REQUIRE_THAT(got[3], WithinAbs(-6.0206003f, 1e-3f));
  }
}

TEST_CASE("dB finite non-positive references retain max-reference sentinel semantics",
          "[db_convert][edge]") {
  const std::vector<float> input{4.0f, 1.0f};
  const auto zero_ref = power_to_db(input, 0.0f, 1e-10f, -1.0f);
  const auto negative_ref = power_to_db(input, -1.0f, 1e-10f, -1.0f);
  REQUIRE_THAT(zero_ref[0], WithinAbs(0.0f, 1e-5f));
  REQUIRE_THAT(negative_ref[0], WithinAbs(0.0f, 1e-5f));
  REQUIRE(zero_ref == negative_ref);
}
