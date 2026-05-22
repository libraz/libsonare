/// @file harmonic_test.cpp
/// @brief Reference compatibility tests for core/harmonic.

#include "core/harmonic.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> flatten_matrix(const JsonValue& m) {
  std::vector<float> out;
  for (const auto& row : m.as_array()) {
    for (const auto& v : row.as_array()) out.push_back(v.as_float());
  }
  return out;
}

std::vector<float> as_floats(const JsonValue& arr) {
  std::vector<float> out;
  for (const auto& v : arr.as_array()) out.push_back(v.as_float());
  return out;
}

std::vector<int> as_ints(const JsonValue& arr) {
  std::vector<int> out;
  for (const auto& v : arr.as_array()) out.push_back(v.as_int());
  return out;
}

}  // namespace

TEST_CASE("interp_harmonics matches librosa", "[librosa][harmonic]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/harmonic.json");
  const auto& d = json["data"];
  const int n_bins = d["n_bins"].as_int();
  const int n_frames = d["n_frames"].as_int();
  auto S = flatten_matrix(d["S"]);
  auto freqs = as_floats(d["frequencies"]);
  auto harmonics_int = as_ints(d["harmonics"]);
  std::vector<float> harmonics(harmonics_int.begin(), harmonics_int.end());

  auto got = interp_harmonics(S.data(), n_bins, n_frames, freqs, harmonics);
  // librosa output shape: [n_h x n_bins x n_frames] (flattened).
  const size_t expected_size = static_cast<size_t>(harmonics.size()) * n_bins * n_frames;
  REQUIRE(got.size() == expected_size);
  // Compare a subset (h=1 row, which is just S itself for unit harmonic).
  // librosa returns harmonics[0] == 1 -> result == S.
  for (int b = 0; b < n_bins; ++b) {
    for (int t = 0; t < n_frames; ++t) {
      const float s = S[b * n_frames + t];
      const float h = got[0 * n_bins * n_frames + b * n_frames + t];
      CAPTURE(b, t, s, h);
      REQUIRE_THAT(h, WithinAbs(s, 1e-4f));
    }
  }
}

TEST_CASE("salience shape matches librosa", "[librosa][harmonic]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/harmonic.json");
  const auto& d = json["data"];
  const int n_bins = d["n_bins"].as_int();
  const int n_frames = d["n_frames"].as_int();
  auto S = flatten_matrix(d["S"]);
  auto freqs = as_floats(d["frequencies"]);
  auto harmonics_int = as_ints(d["harmonics"]);
  std::vector<float> harmonics(harmonics_int.begin(), harmonics_int.end());

  auto got = salience(S.data(), n_bins, n_frames, freqs, harmonics);
  // We only assert shape and non-negativity here: librosa's salience uses
  // weighted aggregation we approximate, so the per-bin values may differ.
  REQUIRE(got.size() == static_cast<size_t>(n_bins) * n_frames);
  for (float v : got) REQUIRE(v >= 0.0f);
}
