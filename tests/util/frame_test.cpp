/// @file frame_test.cpp
/// @brief Unit + librosa parity tests for util.frame.

#include "util/frame.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("frame produces correct number of frames", "[util][frame]") {
  std::vector<float> x(1000, 0.0f);
  // floor((1000 - 256) / 64) + 1 = 12
  REQUIRE(frame_count(x.size(), 256, 64) == 12);
  auto f = frame(x, 256, 64);
  REQUIRE(f.size() == 12 * 256);
}

TEST_CASE("frame returns empty for short input", "[util][frame][edge]") {
  std::vector<float> x(100, 1.0f);
  auto f = frame(x, 256, 64);
  REQUIRE(f.empty());
}

TEST_CASE("frame rejects invalid params", "[util][frame][edge]") {
  std::vector<float> x(100, 0.0f);
  REQUIRE_THROWS_AS(frame(x, 0, 64), std::invalid_argument);
  REQUIRE_THROWS_AS(frame(x, 256, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(frame(nullptr, 100, 256, 64), std::invalid_argument);
}

TEST_CASE("frame matches librosa output exactly", "[librosa][util][frame]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/util_frame.json");
  const auto& d = json["data"];
  int frame_length = d["frame_length"].as_int();
  int hop = d["hop_length"].as_int();
  const auto& input_arr = d["input"].as_array();
  const auto& first_arr = d["first_frame"].as_array();
  const auto& last_arr = d["last_frame"].as_array();

  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto out = frame(input, frame_length, hop);
  const int n_frames = static_cast<int>(out.size() / static_cast<size_t>(frame_length));
  REQUIRE(n_frames > 0);

  // First frame
  for (size_t i = 0; i < first_arr.size(); ++i) {
    REQUIRE_THAT(out[i], WithinAbs(first_arr[i].as_float(), 1e-6f));
  }
  // Last frame
  const size_t last_off = (static_cast<size_t>(n_frames) - 1) * static_cast<size_t>(frame_length);
  for (size_t i = 0; i < last_arr.size(); ++i) {
    REQUIRE_THAT(out[last_off + i], WithinAbs(last_arr[i].as_float(), 1e-6f));
  }
}
