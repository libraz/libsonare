/// @file peak_test.cpp
/// @brief Unit + librosa parity tests for util/peak_pick.

#include "util/peak.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("peak_pick finds inserted peaks", "[util][peak]") {
  std::vector<float> x(100, 0.0f);
  x[10] = 1.0f;
  x[50] = 1.0f;
  x[90] = 1.0f;
  auto peaks = peak_pick(x, 3, 3, 5, 5, 0.1f, 5);
  REQUIRE(peaks.size() == 3);
  REQUIRE(peaks[0] == 10);
  REQUIRE(peaks[1] == 50);
  REQUIRE(peaks[2] == 90);
}

TEST_CASE("peak_pick respects wait window", "[util][peak]") {
  std::vector<float> x(20, 0.0f);
  x[5] = 1.0f;
  x[7] = 1.0f;  // within wait of x[5]
  auto peaks = peak_pick(x, 1, 1, 2, 2, 0.05f, 5);
  REQUIRE(peaks.size() == 1);
  REQUIRE(peaks[0] == 5);
}

TEST_CASE("peak_pick rejects negative params", "[util][peak][edge]") {
  std::vector<float> x(10, 0.0f);
  REQUIRE_THROWS_AS(peak_pick(x, -1, 1, 1, 1, 0.0f, 0), SonareException);
}

TEST_CASE("peak_pick matches librosa output", "[librosa][util][peak]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/peak_pick.json");
  const auto& d = json["data"];
  const auto& input_arr = d["input"].as_array();
  int pre_max = d["pre_max"].as_int();
  int post_max = d["post_max"].as_int();
  int pre_avg = d["pre_avg"].as_int();
  int post_avg = d["post_avg"].as_int();
  float delta = d["delta"].as_float();
  int wait = d["wait"].as_int();

  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto got = peak_pick(input, pre_max, post_max, pre_avg, post_avg, delta, wait);
  const auto& expected_arr = d["expected_peaks"].as_array();

  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected_arr[i].as_int());
    REQUIRE(got[i] == expected_arr[i].as_int());
  }
}

TEST_CASE("peak_pick matches librosa on a plateau-heavy envelope", "[librosa][util][peak]") {
  // Plateaus and a decaying trailing slope with a small wait are exactly where an
  // inclusive-vs-exclusive local-max / moving-average window off-by-one emitted
  // spurious peaks. The C++ window must now match librosa's array slices exactly.
  auto json = JsonReader::parse_file("tests/librosa/reference/peak_pick_plateau.json");
  const auto& d = json["data"];
  const auto& input_arr = d["input"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto got = peak_pick(input, d["pre_max"].as_int(), d["post_max"].as_int(), d["pre_avg"].as_int(),
                       d["post_avg"].as_int(), d["delta"].as_float(), d["wait"].as_int());
  const auto& expected_arr = d["expected_peaks"].as_array();

  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected_arr[i].as_int());
    REQUIRE(got[i] == expected_arr[i].as_int());
  }
}
