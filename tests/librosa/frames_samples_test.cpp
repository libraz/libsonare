/// @file frames_samples_test.cpp
/// @brief Reference compatibility tests for frames_to_samples / samples_to_frames.

#include <catch2/catch_test_macros.hpp>

#include "core/convert.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("frames_to_samples matches librosa", "[librosa][convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/frames_samples.json");
  const auto& data = json["data"];
  int hop = data["hop_length"].as_int();
  int n_fft = data["n_fft"].as_int();

  SECTION("no n_fft") {
    const auto& cases = data["frames_to_samples_no_nfft"].as_array();
    for (const auto& c : cases) {
      int f = c["frames"].as_int();
      int expected = c["samples"].as_int();
      REQUIRE(frames_to_samples(f, hop) == expected);
    }
  }

  SECTION("with n_fft") {
    const auto& cases = data["frames_to_samples_with_nfft"].as_array();
    for (const auto& c : cases) {
      int f = c["frames"].as_int();
      int expected = c["samples"].as_int();
      REQUIRE(frames_to_samples(f, hop, n_fft) == expected);
    }
  }
}

TEST_CASE("samples_to_frames matches librosa", "[librosa][convert]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/frames_samples.json");
  const auto& data = json["data"];
  int hop = data["hop_length"].as_int();
  int n_fft = data["n_fft"].as_int();

  SECTION("no n_fft") {
    const auto& cases = data["samples_to_frames_no_nfft"].as_array();
    for (const auto& c : cases) {
      int s = c["samples"].as_int();
      int expected = c["frames"].as_int();
      REQUIRE(samples_to_frames(s, hop) == expected);
    }
  }

  SECTION("with n_fft") {
    const auto& cases = data["samples_to_frames_with_nfft"].as_array();
    for (const auto& c : cases) {
      int s = c["samples"].as_int();
      int expected = c["frames"].as_int();
      REQUIRE(samples_to_frames(s, hop, n_fft) == expected);
    }
  }
}

TEST_CASE("frames_to_samples vector overload", "[convert][unit]") {
  std::vector<int> frames{0, 1, 5};
  auto out = frames_to_samples(frames, 512);
  REQUIRE(out == std::vector<int>{0, 512, 2560});
}
