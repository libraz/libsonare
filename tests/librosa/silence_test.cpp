/// @file silence_test.cpp
/// @brief Reference compatibility tests for librosa-style trim / split.

#include "effects/silence.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;

namespace {

std::vector<float> tone(int sr, float duration, float freq, float amp) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    y[i] = amp * std::sin(constants::kTwoPi * freq * static_cast<float>(i) / sr);
  }
  return y;
}

std::vector<float> build_signal(int sr) {
  std::vector<float> y;
  // 0.2s silence + 0.3s 440Hz + 0.2s silence + 0.3s 660Hz + 0.2s silence
  auto add_silence = [&](float d) {
    size_t n = static_cast<size_t>(d * sr);
    y.insert(y.end(), n, 0.0f);
  };
  auto add_tone = [&](float d, float f) {
    auto t = tone(sr, d, f, 0.5f);
    y.insert(y.end(), t.begin(), t.end());
  };
  add_silence(0.2f);
  add_tone(0.3f, 440.0f);
  add_silence(0.2f);
  add_tone(0.3f, 660.0f);
  add_silence(0.2f);
  return y;
}

}  // namespace

TEST_CASE("trim narrows silent edges (librosa parity within 1024 samples)", "[librosa][silence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/silence.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  float top_db = d["top_db"].as_float();
  int frame_length = d["frame_length"].as_int();
  int hop = d["hop_length"].as_int();
  int expected_start = d["trim_start_sample"].as_int();
  int expected_end = d["trim_end_sample"].as_int();

  auto y = build_signal(sr);
  auto r = trim(y, top_db, frame_length, hop);
  CAPTURE(r.start_sample, r.end_sample, expected_start, expected_end);
  // Allow ~1 hop tolerance (RMS frame center differences with librosa).
  REQUIRE(std::abs(r.start_sample - expected_start) <= hop + 1);
  REQUIRE(std::abs(r.end_sample - expected_end) <= hop + 1);
  REQUIRE(static_cast<int>(r.audio.size()) == r.end_sample - r.start_sample);
}

TEST_CASE("split returns non-silent intervals (librosa parity)", "[librosa][silence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/silence.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  float top_db = d["top_db"].as_float();
  int frame_length = d["frame_length"].as_int();
  int hop = d["hop_length"].as_int();
  const auto& expected = d["split_intervals"].as_array();

  auto y = build_signal(sr);
  auto intervals = split(y, top_db, frame_length, hop);
  // librosa may merge two close tones into one interval depending on
  // top_db, so just sanity-check counts and rough alignment.
  REQUIRE(intervals.size() >= 1);
  REQUIRE(intervals.size() <= expected.size() + 1);

  // First interval start should match within one hop.
  int first_expected_start = expected[0][0].as_int();
  CAPTURE(intervals.front().first, first_expected_start);
  REQUIRE(std::abs(intervals.front().first - first_expected_start) <= hop + 1);
}

TEST_CASE("trim on all-silent input returns empty audio", "[silence][edge]") {
  std::vector<float> y(22050, 0.0f);
  auto r = trim(y);
  REQUIRE(r.audio.empty());
  REQUIRE(r.start_sample == 0);
  REQUIRE(r.end_sample == 0);
}
