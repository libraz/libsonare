/// @file preemphasis_test.cpp
/// @brief Reference compatibility tests for preemphasis / deemphasis.

#include "effects/preemphasis.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("preemphasis matches librosa", "[librosa][preemphasis]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/preemphasis.json");
  const auto& d = json["data"];
  float coef = d["coef"].as_float();
  const auto& input_arr = d["input"].as_array();
  const auto& expected_arr = d["preemphasized"].as_array();

  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto got = preemphasis(input, coef);
  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    float e = expected_arr[i].as_float();
    CAPTURE(i, got[i], e);
    REQUIRE_THAT(got[i], WithinAbs(e, 1e-5f));
  }
}

TEST_CASE("deemphasis inverts preemphasis (librosa)", "[librosa][preemphasis]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/preemphasis.json");
  const auto& d = json["data"];
  float coef = d["coef"].as_float();
  const auto& expected_arr = d["deemphasized"].as_array();
  const auto& pre_arr = d["preemphasized"].as_array();

  std::vector<float> pre;
  pre.reserve(pre_arr.size());
  for (const auto& v : pre_arr) pre.push_back(v.as_float());

  auto got = deemphasis(pre, coef);
  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    float e = expected_arr[i].as_float();
    CAPTURE(i, got[i], e);
    REQUIRE_THAT(got[i], WithinAbs(e, 1e-4f));
  }
}

TEST_CASE("preemphasis handles edges", "[preemphasis][unit]") {
  std::vector<float> empty;
  REQUIRE(preemphasis(empty).empty());
  std::vector<float> one{0.5f};
  // Single-sample input with explicit zi: y[0] = x[0] + zi.
  auto r = preemphasis(one, 0.97f, 0.1f);
  REQUIRE(r.size() == 1);
  REQUIRE_THAT(r[0], WithinAbs(0.5f + 0.1f, 1e-6f));
}

TEST_CASE("preemphasis validates finite parameters before processing",
          "[preemphasis][validation]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<float> empty;
  const std::vector<float> input{1.0f, 2.0f, 3.0f};

  SECTION("both filters reject non-finite coefficients, including on empty input") {
    for (const float coef :
         {nan, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      CAPTURE(coef);
      REQUIRE_THROWS_AS(preemphasis(empty, coef), SonareException);
      REQUIRE_THROWS_AS(deemphasis(empty, coef), SonareException);
    }
  }

  SECTION("both filters reject a non-finite engaged state") {
    for (const float zi :
         {nan, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      CAPTURE(zi);
      REQUIRE_THROWS_AS(preemphasis(empty, 0.5f, zi), SonareException);
      REQUIRE_THROWS_AS(deemphasis(empty, 0.5f, zi), SonareException);
    }
  }

  SECTION("null input is reported before non-finite parameters") {
    try {
      static_cast<void>(preemphasis(nullptr, 1, nan, nan));
      FAIL("Expected null input to be rejected first");
    } catch (const SonareException& error) {
      REQUIRE(std::string(error.what()).find("null input") != std::string::npos);
    }
  }

  SECTION("finite coefficients outside the usual unit interval remain valid") {
    const auto pre = preemphasis(input, 1.5f, 0.25f);
    const auto de = deemphasis(input, -1.5f, 0.25f);
    REQUIRE(pre.size() == input.size());
    REQUIRE(de.size() == input.size());
    for (const float value : pre) REQUIRE(std::isfinite(value));
    for (const float value : de) REQUIRE(std::isfinite(value));
  }

  SECTION("explicit finite state keeps the reference recurrence") {
    const auto pre = preemphasis(input, 0.5f, 0.25f);
    const auto de = deemphasis(input, 0.5f, 0.25f);
    REQUIRE_THAT(pre[0], WithinAbs(1.25f, 1e-6f));
    REQUIRE_THAT(pre[1], WithinAbs(1.5f, 1e-6f));
    REQUIRE_THAT(pre[2], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(de[0], WithinAbs(1.25f, 1e-6f));
    REQUIRE_THAT(de[1], WithinAbs(2.625f, 1e-6f));
    REQUIRE_THAT(de[2], WithinAbs(4.3125f, 1e-6f));
  }
}
