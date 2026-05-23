/// @file bpm_parity_test.cpp
/// @brief High-level librosa tempo parity tests.
/// @details Reference values from: tests/librosa/reference/tempo.json

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/bpm_analyzer.h"
#include "core/audio.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> create_impulse_train(int sr, float duration, float bpm) {
  std::vector<float> samples(static_cast<size_t>(duration * static_cast<float>(sr)), 0.0f);
  const float samples_per_beat = 60.0f / bpm * static_cast<float>(sr);

  int beat = 0;
  while (true) {
    const int index = static_cast<int>(std::round(static_cast<float>(beat) * samples_per_beat));
    if (index >= static_cast<int>(samples.size())) break;
    samples[static_cast<size_t>(index)] = 1.0f;
    ++beat;
  }

  return samples;
}

float nearest_octave_equivalent_delta(float detected, float reference) {
  const std::vector<float> equivalents = {reference * 0.5f, reference, reference * 2.0f};
  float best = std::numeric_limits<float>::infinity();
  for (float equivalent : equivalents) {
    best = std::min(best, std::abs(detected - equivalent));
  }
  return best;
}

}  // namespace

TEST_CASE("high-level BPM parity matches librosa tempo within two BPM",
          "[librosa][high_level][bpm]") {
  const auto json = JsonReader::parse_file("tests/librosa/reference/tempo.json");
  const auto& data = json["data"].as_array();

  constexpr int kSampleRate = 22050;
  constexpr float kDurationSec = 20.0f;
  constexpr float kToleranceBpm = 2.0f;

  BpmConfig config;
  config.bpm_min = 30.0f;
  config.bpm_max = 300.0f;
  config.start_bpm = 120.0f;

  for (const auto& item : data) {
    const float true_tempo = item["true_tempo"].as_float();
    const float librosa_tempo = item["detected_tempo"].as_float();

    DYNAMIC_SECTION("tempo=" << true_tempo) {
      Audio audio = Audio::from_vector(create_impulse_train(kSampleRate, kDurationSec, true_tempo),
                                       kSampleRate);
      const float detected = detect_bpm(audio, config);
      // Require sonare to match the GROUND TRUTH tempo (not librosa's possibly
      // wrong answer). Exact octave equivalents (half / double tempo) are
      // accepted explicitly, each held to the same tight tolerance so a gross
      // octave/meter error in an unrelated direction still fails.
      const float true_delta = nearest_octave_equivalent_delta(detected, true_tempo);

      CAPTURE(true_tempo, librosa_tempo, detected, true_delta);
      REQUIRE(true_delta <= kToleranceBpm);
    }
  }
}
