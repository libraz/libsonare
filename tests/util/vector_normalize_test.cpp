/// @file vector_normalize_test.cpp
/// @brief Unit + librosa parity tests for util/vector_normalize.

#include "util/vector_normalize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("normalize Inf norm puts peak at 1", "[util][normalize]") {
  std::vector<float> x{1.0f, -2.0f, 3.0f, -4.0f};
  auto r = normalize(x, NormType::Inf);
  REQUIRE_THAT(r[0], WithinAbs(0.25f, 1e-6f));
  REQUIRE_THAT(r[3], WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("normalize L2 norm produces unit vector", "[util][normalize]") {
  std::vector<float> x{3.0f, 4.0f};
  auto r = normalize(x, NormType::L2);
  REQUIRE_THAT(r[0], WithinAbs(0.6f, 1e-6f));
  REQUIRE_THAT(r[1], WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("normalize leaves zero vectors unchanged", "[util][normalize][edge]") {
  std::vector<float> x(5, 0.0f);
  auto r = normalize(x, NormType::Inf);
  for (float v : r) REQUIRE(v == 0.0f);
}

TEST_CASE("vector normalize matches librosa (Inf/L1/L2)", "[librosa][util][normalize]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/vector_normalize.json");
  const auto& v = json["data"]["vector"];
  const auto& input_arr = v["input"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& e : input_arr) input.push_back(e.as_float());

  auto check = [&](const std::string& key, NormType type) {
    const auto& expected_arr = v[key].as_array();
    auto got = normalize(input, type);
    REQUIRE(got.size() == expected_arr.size());
    for (size_t i = 0; i < got.size(); ++i) {
      REQUIRE_THAT(got[i], WithinAbs(expected_arr[i].as_float(), 1e-5f));
    }
  };
  check("inf_norm", NormType::Inf);
  check("l1_norm", NormType::L1);
  check("l2_norm", NormType::L2);
}

TEST_CASE("matrix normalize matches librosa along both axes", "[librosa][util][normalize]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/vector_normalize.json");
  const auto& m = json["data"]["matrix"];
  int rows = m["rows"].as_int();
  int cols = m["cols"].as_int();
  const auto& input_arr = m["input_flat"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& e : input_arr) input.push_back(e.as_float());

  auto check_axis = [&](int axis, const std::string& key) {
    const auto& expected_arr = m[key].as_array();
    auto got = normalize_matrix(input.data(), rows, cols, axis, NormType::Inf);
    REQUIRE(got.size() == expected_arr.size());
    for (size_t i = 0; i < got.size(); ++i) {
      REQUIRE_THAT(got[i], WithinAbs(expected_arr[i].as_float(), 1e-5f));
    }
  };
  check_axis(1, "axis1_inf_norm_flat");
  check_axis(0, "axis0_inf_norm_flat");
}
