/// @file remix_test.cpp
/// @brief Reference compatibility tests for effects::remix.

#include "effects/remix.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> load_floats(const sonare::test::JsonValue& arr) {
  std::vector<float> out;
  out.reserve(arr.size());
  for (const auto& v : arr.as_array()) out.push_back(v.as_float());
  return out;
}

std::vector<std::pair<int, int>> load_intervals(const sonare::test::JsonValue& arr) {
  std::vector<std::pair<int, int>> out;
  for (const auto& iv : arr.as_array()) {
    out.emplace_back(iv[0].as_int(), iv[1].as_int());
  }
  return out;
}

}  // namespace

TEST_CASE("remix matches librosa (no alignment)", "[librosa][remix]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/remix.json");
  const auto& d = json["data"];
  auto y = load_floats(d["input"]);
  auto intervals = load_intervals(d["intervals"]);
  auto expected = load_floats(d["unaligned"]);

  auto got = remix(y, intervals, /*align_zeros=*/false);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-5f));
  }
}

TEST_CASE("remix matches librosa (align_zeros=true)", "[librosa][remix]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/remix.json");
  const auto& d = json["data"];
  auto y = load_floats(d["input"]);
  auto intervals = load_intervals(d["intervals"]);
  auto expected = load_floats(d["aligned"]);

  auto got = remix(y, intervals, /*align_zeros=*/true);
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-5f));
  }
}
