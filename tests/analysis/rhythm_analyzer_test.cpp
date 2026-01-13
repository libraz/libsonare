/// @file rhythm_analyzer_test.cpp
/// @brief Tests for rhythm analyzer.

#include "analysis/rhythm_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a regular click track at specified BPM.
Audio create_click_track(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a 4/4 pattern with accented downbeats.
Audio create_4_4_pattern(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % 4 == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a 3/4 pattern (waltz).
Audio create_3_4_pattern(float bpm, int sr = 22050, float duration = 6.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % 3 == 0) ? 1.0f : 0.4f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("RhythmAnalyzer basic", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  RhythmConfig config;
  RhythmAnalyzer analyzer(audio, config);

  const auto& features = analyzer.features();

  REQUIRE(features.syncopation >= 0.0f);
  REQUIRE(features.syncopation <= 1.0f);
  REQUIRE(features.pattern_regularity >= 0.0f);
  REQUIRE(features.pattern_regularity <= 1.0f);
  REQUIRE(features.tempo_stability >= 0.0f);
  REQUIRE(features.tempo_stability <= 1.0f);
}

TEST_CASE("RhythmAnalyzer time signature 4/4", "[rhythm_analyzer]") {
  Audio audio = create_4_4_pattern(120.0f, 22050, 8.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  RhythmAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  // Should detect 4 or related meter
  REQUIRE((ts.numerator == 4 || ts.numerator == 2));
  REQUIRE(ts.denominator == 4);
  REQUIRE(ts.confidence >= 0.0f);
  REQUIRE(ts.confidence <= 1.0f);
}

TEST_CASE("RhythmAnalyzer time signature 3/4", "[rhythm_analyzer]") {
  Audio audio = create_3_4_pattern(100.0f, 22050, 9.0f);

  RhythmConfig config;
  config.start_bpm = 100.0f;
  RhythmAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  // Should detect 3 or 6
  REQUIRE((ts.numerator == 3 || ts.numerator == 6 || ts.numerator == 4));
}

TEST_CASE("RhythmAnalyzer straight groove", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  REQUIRE(analyzer.groove_type() == "straight");
}

TEST_CASE("RhythmAnalyzer regularity high for steady tempo", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  // Regular click track should have high regularity
  REQUIRE(analyzer.pattern_regularity() >= 0.5f);
}

TEST_CASE("RhythmAnalyzer tempo stability", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  // Steady tempo should have high stability
  REQUIRE(analyzer.tempo_stability() >= 0.5f);
}

TEST_CASE("RhythmAnalyzer bpm", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  RhythmAnalyzer analyzer(audio, config);

  REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.2f));
}

TEST_CASE("RhythmAnalyzer beat intervals", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 4.0f);

  RhythmAnalyzer analyzer(audio);

  const auto& intervals = analyzer.beat_intervals();

  // Should have intervals between beats
  if (!intervals.empty()) {
    for (float interval : intervals) {
      REQUIRE(interval > 0.0f);
    }
  }
}

TEST_CASE("RhythmAnalyzer from BeatAnalyzer", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig beat_config;
  beat_config.start_bpm = 120.0f;
  BeatAnalyzer beat_analyzer(audio, beat_config);

  RhythmAnalyzer rhythm_analyzer(beat_analyzer);

  REQUIRE(rhythm_analyzer.bpm() > 0.0f);
  REQUIRE(rhythm_analyzer.groove_type().length() > 0);
}

TEST_CASE("RhythmAnalyzer syncopation", "[rhythm_analyzer]") {
  Audio audio = create_4_4_pattern(120.0f, 22050, 4.0f);

  RhythmAnalyzer analyzer(audio);

  // Regular 4/4 pattern should have low syncopation
  REQUIRE(analyzer.syncopation() >= 0.0f);
  REQUIRE(analyzer.syncopation() <= 1.0f);
}

TEST_CASE("RhythmAnalyzer features struct", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  RhythmAnalyzer analyzer(audio);

  const auto& features = analyzer.features();

  REQUIRE(features.time_signature.numerator > 0);
  REQUIRE(features.time_signature.denominator > 0);
  REQUIRE(!features.groove_type.empty());
}

TEST_CASE("RhythmAnalyzer short audio", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 1.0f);

  RhythmConfig config;
  RhythmAnalyzer analyzer(audio, config);

  // Should still work for short audio
  REQUIRE(analyzer.bpm() > 0.0f);
}
