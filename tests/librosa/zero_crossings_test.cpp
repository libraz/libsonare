/// @file zero_crossings_test.cpp
/// @brief Reference compatibility tests for zero_crossings (raw indices).

#include "feature/spectral.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> load_floats(const sonare::test::JsonValue& arr) {
  std::vector<float> out;
  out.reserve(arr.size());
  for (const auto& v : arr.as_array()) out.push_back(v.as_float());
  return out;
}

std::vector<int> load_ints(const sonare::test::JsonValue& arr) {
  std::vector<int> out;
  out.reserve(arr.size());
  for (const auto& v : arr.as_array()) out.push_back(v.as_int());
  return out;
}

}  // namespace

TEST_CASE("zero_crossings matches librosa (default)", "[librosa][zero_crossings]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/zero_crossings.json");
  const auto& d = json["data"];
  auto y = load_floats(d["input"]);
  auto expected = load_ints(d["indices_default"]);
  float threshold = d["threshold"].as_float();
  auto got = zero_crossings(y, threshold, /*ref_magnitude=*/false,
                            /*pad=*/true, /*zero_pos=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE(got[i] == expected[i]);
  }
}

TEST_CASE("zero_crossings matches librosa (no pad)", "[librosa][zero_crossings]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/zero_crossings.json");
  const auto& d = json["data"];
  auto y = load_floats(d["input"]);
  auto expected = load_ints(d["indices_no_pad"]);
  float threshold = d["threshold"].as_float();
  auto got = zero_crossings(y, threshold, /*ref_magnitude=*/false,
                            /*pad=*/false, /*zero_pos=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE(got[i] == expected[i]);
  }
}

TEST_CASE("zero_crossings small sanity input matches librosa", "[librosa][zero_crossings]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/zero_crossings.json");
  const auto& d = json["data"];
  auto y = load_floats(d["small_input"]);
  auto expected = load_ints(d["small_indices"]);
  auto got = zero_crossings(y, /*threshold=*/0.0f, /*ref_magnitude=*/false,
                            /*pad=*/true, /*zero_pos=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE(got[i] == expected[i]);
  }
}
