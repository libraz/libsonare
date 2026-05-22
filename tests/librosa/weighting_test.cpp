/// @file weighting_test.cpp
/// @brief Reference compatibility tests for A/B/C/D / frequency / perceptual weighting.

#include "core/weighting.h"

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

void check_curve(const std::vector<float>& got, const std::vector<float>& expected, float tol) {
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], tol));
  }
}

}  // namespace

TEST_CASE("A/B/C/D weighting matches librosa", "[librosa][weighting]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/weighting.json");
  const auto& d = json["data"];
  auto freqs = load_floats(d["freqs"]);

  // Pass a very negative min_db to disable clipping (matches librosa's default
  // min_db=-80 but our test points are all above that floor).
  const float min_db = -1e9f;

  auto a = A_weighting(freqs, min_db);
  auto b = B_weighting(freqs, min_db);
  auto c = C_weighting(freqs, min_db);
  auto dd = D_weighting(freqs, min_db);

  check_curve(a, load_floats(d["A"]), 1e-3f);
  check_curve(b, load_floats(d["B"]), 1e-3f);
  check_curve(c, load_floats(d["C"]), 1e-3f);
  check_curve(dd, load_floats(d["D"]), 1e-3f);

  auto fa = frequency_weighting(freqs, "A", min_db);
  check_curve(fa, load_floats(d["freq_A"]), 1e-3f);
}

TEST_CASE("perceptual_weighting matches librosa", "[librosa][weighting]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/weighting.json");
  const auto& d = json["data"];
  const auto& p = d["perceptual"];
  int n_bins = p["n_bins"].as_int();
  int n_frames = p["n_frames"].as_int();
  auto bin_freqs = load_floats(p["bin_freqs"]);
  auto S = load_floats(p["S_flat"]);
  auto expected = load_floats(p["expected_flat"]);

  auto got = perceptual_weighting(S.data(), n_bins, n_frames, bin_freqs, "A");
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected[i]);
    REQUIRE_THAT(got[i], WithinAbs(expected[i], 1e-2f));
  }
}
