/// @file pitch_utilities_test.cpp
/// @brief librosa parity test for pitch_tuning / estimate_tuning.
/// @details Reference: tests/librosa/reference/pitch_utilities.json

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;

TEST_CASE("pitch_tuning matches librosa within tolerance", "[librosa][pitch_utilities]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/pitch_utilities.json");
  const auto& d = json["data"];

  const auto& freqs_arr = d["known_frequencies"].as_array();
  std::vector<float> freqs;
  freqs.reserve(freqs_arr.size());
  for (const auto& v : freqs_arr) freqs.push_back(v.as_float());

  float got = pitch_tuning(freqs, 0.01f, 12);
  float expected = d["pitch_tuning"].as_float();
  CAPTURE(got, expected);
  REQUIRE(std::abs(got - expected) < 0.05f);
}

TEST_CASE("estimate_tuning matches librosa within loose tolerance", "[librosa][pitch_utilities]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/pitch_utilities.json");
  const auto& d = json["data"];

  int sr = d["sr"].as_int();
  int n_fft = d["n_fft"].as_int();
  int hop_length = d["hop_length"].as_int();

  // Rebuild the same 1s mixture of 440 / 660 / 880 Hz.
  const size_t n = static_cast<size_t>(sr);
  std::vector<float> y(n);
  const double tp = static_cast<double>(constants::kTwoPi);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(
        (std::sin(tp * 440.0 * t) + std::sin(tp * 660.0 * t) + std::sin(tp * 880.0 * t)) / 3.0);
  }
  Audio audio = Audio::from_vector(std::move(y), sr);

  float got = estimate_tuning(audio, n_fft, hop_length);
  float expected = d["estimate_tuning"].as_float();
  CAPTURE(got, expected);
  // estimate_tuning uses median-magnitude filtering which differs from librosa;
  // accept ±0.1 (10 cents) deviation.
  REQUIRE(std::abs(got - expected) < 0.1f);
}
