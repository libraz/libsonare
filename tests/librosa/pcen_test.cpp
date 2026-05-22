/// @file pcen_test.cpp
/// @brief librosa parity test for PCEN.
/// @details Reference: tests/librosa/reference/pcen.json

#include "core/pcen.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("pcen matches librosa reference", "[librosa][pcen]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/pcen.json");
  const auto& d = json["data"];

  int sr = d["sr"].as_int();
  int hop_length = d["hop_length"].as_int();
  int n_bins = d["n_bins"].as_int();
  int n_frames = d["n_frames"].as_int();
  float time_constant = d["time_constant"].as_float();
  float gain = d["gain"].as_float();
  float bias = d["bias"].as_float();
  float power = d["power"].as_float();
  float eps = d["eps"].as_float();

  const auto& s_arr = d["S_flat"].as_array();
  const auto& exp_arr = d["expected_flat"].as_array();
  REQUIRE(s_arr.size() == static_cast<size_t>(n_bins * n_frames));
  REQUIRE(exp_arr.size() == s_arr.size());

  std::vector<float> S;
  S.reserve(s_arr.size());
  for (const auto& v : s_arr) S.push_back(v.as_float());

  PcenConfig cfg;
  cfg.sr = sr;
  cfg.hop_length = hop_length;
  cfg.time_constant = time_constant;
  cfg.gain = gain;
  cfg.bias = bias;
  cfg.power = power;
  cfg.eps = eps;

  auto got = pcen(S.data(), n_bins, n_frames, cfg);
  REQUIRE(got.size() == exp_arr.size());

  double max_abs_diff = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    float e = exp_arr[i].as_float();
    double diff = std::abs(static_cast<double>(got[i]) - static_cast<double>(e));
    max_abs_diff = std::max(max_abs_diff, diff);
  }
  CAPTURE(max_abs_diff);
  REQUIRE(max_abs_diff < 1e-3);
}
