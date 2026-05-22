/// @file sequence_test.cpp
/// @brief Reference compatibility tests for util/sequence.

#include "util/sequence.h"

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

}  // namespace

TEST_CASE("dtw distance matches librosa", "[librosa][sequence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/sequence.json");
  const auto& d = json["data"];
  const int X_rows = d["dtw_X_rows"].as_int();
  const int X_cols = d["dtw_X_cols"].as_int();
  const int Y_cols = d["dtw_Y_cols"].as_int();
  auto X = flatten_matrix(d["dtw_X"]);
  auto Y = flatten_matrix(d["dtw_Y"]);
  const float expected = d["dtw_distance"].as_float();

  auto r = dtw(X.data(), X_rows, X_cols, Y.data(), X_rows, Y_cols, "euclidean");
  CAPTURE(r.distance, expected);
  REQUIRE_THAT(r.distance, WithinAbs(expected, 1e-3f));
  // Path endpoints (with default symmetric P0).
  REQUIRE(r.path.front().first == 0);
  REQUIRE(r.path.front().second == 0);
  REQUIRE(r.path.back().first == X_cols - 1);
  REQUIRE(r.path.back().second == Y_cols - 1);
}

TEST_CASE("viterbi path matches librosa", "[librosa][sequence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/sequence.json");
  const auto& d = json["data"];
  auto log_emit = flatten_matrix(d["viterbi_log_emission"]);
  auto trans = flatten_matrix(d["viterbi_transition"]);
  const auto& expected_path = d["viterbi_path"].as_array();

  const int n_states = static_cast<int>(d["viterbi_log_emission"].size());
  const int n_steps = static_cast<int>(d["viterbi_log_emission"][0].size());
  auto path = viterbi(log_emit.data(), n_states, n_steps, trans.data(), nullptr);
  REQUIRE(static_cast<int>(path.size()) == n_steps);
  for (int t = 0; t < n_steps; ++t) {
    CAPTURE(t, path[t], expected_path[t].as_int());
    REQUIRE(path[t] == expected_path[t].as_int());
  }
}
