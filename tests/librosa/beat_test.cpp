/// @file beat_test.cpp
/// @brief Reference compatibility tests for beat tracking.
/// @details Reference values from: tests/librosa/reference/beat.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "core/audio.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> create_impulse_train(int sr, float duration, float bpm) {
  std::vector<float> y(static_cast<size_t>(duration * sr), 0.0f);
  int samples_per_beat = static_cast<int>(60.0f / bpm * static_cast<float>(sr));
  for (size_t i = 0; i < y.size(); i += static_cast<size_t>(samples_per_beat)) {
    y[i] = 1.0f;
  }
  return y;
}

}  // namespace

TEST_CASE("beat tracking reference compatibility", "[beat][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/beat.json");
  const auto& item = json["data"];

  int sr = item["sr"].as_int();
  float true_bpm = item["true_bpm"].as_float();
  float expected_tempo = item["detected_tempo"].as_float();
  int expected_n_beats = item["n_beats"].as_int();
  const auto& expected_times = item["beat_times"].as_array();

  auto samples = create_impulse_train(sr, 10.0f, true_bpm);
  Audio audio = Audio::from_vector(std::move(samples), sr);

  BeatConfig config;
  config.start_bpm = true_bpm;
  config.bpm_min = 60.0f;
  config.bpm_max = 180.0f;
  config.hop_length = 512;

  BeatAnalyzer analyzer(audio, config);
  std::vector<float> times = analyzer.beat_times();

  CAPTURE(analyzer.bpm(), expected_tempo, times.size(), expected_n_beats);
  REQUIRE_THAT(analyzer.bpm(), WithinRel(expected_tempo, 0.05f));
  REQUIRE(std::abs(static_cast<int>(times.size()) - expected_n_beats) <= 1);

  size_t compare_count = std::min(times.size(), expected_times.size());
  REQUIRE(compare_count >= 4);
  for (size_t i = 0; i < compare_count; ++i) {
    CAPTURE(i, times[i], expected_times[i].as_float());
    REQUIRE_THAT(times[i], WithinAbs(expected_times[i].as_float(), 0.08f));
  }
}
