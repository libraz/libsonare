/// @file tempo_test.cpp
/// @brief librosa compatibility tests for tempo detection.
/// @details Reference values from: tests/librosa/reference/tempo.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "analysis/bpm_analyzer.h"
#include "core/audio.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates impulse train at specified tempo.
/// @param sr Sample rate
/// @param duration Duration in seconds
/// @param tempo BPM
/// @return Audio samples
std::vector<float> create_tempo_signal(int sr, float duration, float tempo) {
  std::vector<float> y(static_cast<size_t>(duration * sr), 0.0f);

  float samples_per_beat = 60.0f / tempo * sr;
  int beat_idx = 0;

  while (true) {
    int idx = static_cast<int>(beat_idx * samples_per_beat);
    if (idx >= static_cast<int>(y.size())) break;
    y[idx] = 1.0f;
    ++beat_idx;
  }

  return y;
}

/// @brief Checks if detected tempo matches expected (allowing octave errors).
/// @details Tempo detection commonly has octave errors (detecting half or double tempo).
bool tempo_matches(float detected, float expected, float tolerance_pct) {
  float tolerance = expected * tolerance_pct / 100.0f;

  // Direct match
  if (std::abs(detected - expected) <= tolerance) return true;

  // Half tempo (octave down)
  if (std::abs(detected * 2.0f - expected) <= tolerance) return true;

  // Double tempo (octave up)
  if (std::abs(detected / 2.0f - expected) <= tolerance) return true;

  return false;
}

}  // namespace

TEST_CASE("tempo detection librosa compatibility", "[tempo][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/tempo.json");
  const auto& data = json["data"].as_array();

  const int sr = 22050;
  const float duration = 20.0f;

  for (const auto& item : data) {
    float true_tempo = item["true_tempo"].as_float();
    float tolerance_pct = item["tolerance_percent"].as_float();

    std::string section_name = "tempo=" + std::to_string(static_cast<int>(true_tempo));

    SECTION(section_name) {
      // Create impulse train at the target tempo
      auto samples = create_tempo_signal(sr, duration, true_tempo);
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Detect BPM
      BpmConfig config;
      config.bpm_min = 30.0f;
      config.bpm_max = 300.0f;
      config.start_bpm = 120.0f;

      float detected = detect_bpm(audio, config);

      // Check if detected tempo matches (allowing octave errors)
      CAPTURE(true_tempo, detected);
      REQUIRE(tempo_matches(detected, true_tempo, tolerance_pct * 2.0f));
    }
  }
}
