/// @file tonnetz_test.cpp
/// @brief librosa parity test for tonnetz.
/// @details Reference: tests/librosa/reference/tonnetz.json

#include "feature/tonnetz.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("tonnetz matches librosa reference", "[librosa][tonnetz]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/tonnetz.json");
  const auto& d = json["data"];

  const auto& chroma_shape = d["chroma_shape"].as_array();
  int n_chroma = chroma_shape[0].as_int();
  int n_frames = chroma_shape[1].as_int();
  REQUIRE(n_chroma == 12);

  const auto& chroma_arr = d["chroma_flat"].as_array();
  REQUIRE(chroma_arr.size() == static_cast<size_t>(n_chroma * n_frames));

  std::vector<float> chroma_in;
  chroma_in.reserve(chroma_arr.size());
  for (const auto& v : chroma_arr) chroma_in.push_back(v.as_float());

  auto got = tonnetz(chroma_in.data(), n_chroma, n_frames);
  const auto& expected = d["tonnetz_flat"].as_array();
  REQUIRE(got.size() == expected.size());

  double max_abs_diff = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    float e = expected[i].as_float();
    double diff = std::abs(static_cast<double>(got[i]) - static_cast<double>(e));
    max_abs_diff = std::max(max_abs_diff, diff);
  }
  CAPTURE(max_abs_diff);
  // librosa uses a slightly different basis ordering convention; tolerance
  // 1e-4 accommodates float accumulation noise.
  REQUIRE(max_abs_diff < 1e-4);
}
