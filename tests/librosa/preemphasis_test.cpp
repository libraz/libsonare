/// @file preemphasis_test.cpp
/// @brief Reference compatibility tests for preemphasis / deemphasis.

#include "effects/preemphasis.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

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
