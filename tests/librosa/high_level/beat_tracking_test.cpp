/// @file beat_tracking_test.cpp
/// @brief High-level librosa beat tracking parity tests.
/// @details Reference values from: tests/librosa/reference/beat.json

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "core/audio.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> create_impulse_train(int sr, float duration, float bpm) {
  std::vector<float> samples(static_cast<size_t>(duration * static_cast<float>(sr)), 0.0f);
  const int samples_per_beat = static_cast<int>(60.0f / bpm * static_cast<float>(sr));

  for (size_t index = 0; index < samples.size(); index += static_cast<size_t>(samples_per_beat)) {
    samples[index] = 1.0f;
  }

  return samples;
}

int time_to_frame(float time, int sr, int hop_length) {
  return static_cast<int>(
      std::round(time * static_cast<float>(sr) / static_cast<float>(hop_length)));
}

}  // namespace

TEST_CASE("high-level beat tracking parity matches librosa beat frames",
          "[librosa][high_level][beat]") {
  const auto json = JsonReader::parse_file("tests/librosa/reference/beat.json");
  const auto& item = json["data"];

  const int sr = item["sr"].as_int();
  const float true_bpm = item["true_bpm"].as_float();
  const auto& expected_times = item["beat_times"].as_array();

  BeatConfig config;
  config.start_bpm = true_bpm;
  config.bpm_min = 60.0f;
  config.bpm_max = 180.0f;
  config.hop_length = 512;

  Audio audio = Audio::from_vector(create_impulse_train(sr, 10.0f, true_bpm), sr);
  BeatAnalyzer analyzer(audio, config);

  const std::vector<int> frames = analyzer.beat_frames();
  REQUIRE(std::abs(static_cast<int>(frames.size()) - static_cast<int>(expected_times.size())) <= 1);

  const size_t compare_count = std::min(frames.size(), expected_times.size());
  REQUIRE(compare_count >= 4);
  for (size_t i = 0; i < compare_count; ++i) {
    const int expected_frame = time_to_frame(expected_times[i].as_float(), sr, config.hop_length);

    CAPTURE(i, frames[i], expected_frame);
    REQUIRE(std::abs(frames[i] - expected_frame) <= 2);
  }
}
