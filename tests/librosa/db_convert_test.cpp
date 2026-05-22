/// @file db_convert_test.cpp
/// @brief Reference compatibility tests for standalone dB conversions.

#include "core/db_convert.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

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
  REQUIRE_THROWS_AS(power_to_db(nullptr, 5), std::invalid_argument);
  std::vector<float> v{0.5f};
  REQUIRE_THROWS_AS(power_to_db(v, 1.0f, 0.0f, 80.0f), std::invalid_argument);
}
