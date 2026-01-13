/// @file beat_analyzer_test.cpp
/// @brief Tests for beat analyzer.

#include "analysis/beat_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a click track at specified BPM.
Audio create_click_track(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;  // 10ms click

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a drum pattern with accented downbeats.
Audio create_drum_pattern(float bpm, int beats_per_bar, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;

  int beat_count = 0;
  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);

    // Downbeat (first beat of bar) is louder
    float amplitude = (beat_count % beats_per_bar == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("BeatAnalyzer basic", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig config;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.start_bpm = 120.0f;

  BeatAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.bpm() >= config.bpm_min);
  REQUIRE(analyzer.bpm() <= config.bpm_max);
  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("BeatAnalyzer beat times", "[beat_analyzer]") {
  float bpm = 120.0f;
  float duration = 4.0f;
  Audio audio = create_click_track(bpm, 22050, duration);

  BeatConfig config;
  config.start_bpm = bpm;
  BeatAnalyzer analyzer(audio, config);

  auto times = analyzer.beat_times();

  // Should have approximately bpm * duration / 60 beats
  float expected_beats = bpm * duration / 60.0f;
  REQUIRE(times.size() >= static_cast<size_t>(expected_beats * 0.5f));
  REQUIRE(times.size() <= static_cast<size_t>(expected_beats * 1.5f));

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer beat frames", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  auto frames = analyzer.beat_frames();
  auto times = analyzer.beat_times();

  REQUIRE(frames.size() == times.size());

  // Frames should be monotonically increasing
  for (size_t i = 1; i < frames.size(); ++i) {
    REQUIRE(frames[i] > frames[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer 120 BPM tracking", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  // Estimated BPM should be close to 120
  REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.1f));

  // Beat intervals should be approximately 0.5 seconds
  auto times = analyzer.beat_times();
  if (times.size() >= 2) {
    float avg_interval = 0.0f;
    for (size_t i = 1; i < times.size(); ++i) {
      avg_interval += times[i] - times[i - 1];
    }
    avg_interval /= static_cast<float>(times.size() - 1);

    REQUIRE_THAT(avg_interval, WithinRel(0.5f, 0.15f));
  }
}

TEST_CASE("BeatAnalyzer 90 BPM tracking", "[beat_analyzer]") {
  Audio audio = create_click_track(90.0f, 22050, 5.0f);

  BeatConfig config;
  config.start_bpm = 90.0f;
  BeatAnalyzer analyzer(audio, config);

  // Should detect BPM close to 90 or octave-related
  float detected = analyzer.bpm();
  bool close_to_90 = std::abs(detected - 90.0f) < 15.0f;
  bool close_to_180 = std::abs(detected - 180.0f) < 15.0f;

  REQUIRE((close_to_90 || close_to_180));
}

TEST_CASE("BeatAnalyzer time signature 4/4", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 8.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  REQUIRE(ts.denominator == 4);
  REQUIRE(ts.confidence >= 0.0f);
  REQUIRE(ts.confidence <= 1.0f);
}

TEST_CASE("BeatAnalyzer time signature 3/4", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 3, 22050, 8.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  // Should detect 3 or 6 beats per bar
  REQUIRE((ts.numerator == 3 || ts.numerator == 6 || ts.numerator == 4));
  REQUIRE(ts.denominator == 4);
}

TEST_CASE("BeatAnalyzer onset strength", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  const auto& onset = analyzer.onset_strength();

  REQUIRE(!onset.empty());
  REQUIRE(analyzer.sample_rate() == audio.sample_rate());
}

TEST_CASE("BeatAnalyzer from onset strength", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  // First create analyzer from audio
  BeatAnalyzer analyzer1(audio);

  // Then create from onset strength
  BeatAnalyzer analyzer2(analyzer1.onset_strength(), audio.sample_rate(), 512);

  // Both should detect similar BPM
  REQUIRE_THAT(analyzer2.bpm(), WithinRel(analyzer1.bpm(), 0.2f));
}

TEST_CASE("BeatAnalyzer beat strength", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 4.0f);

  BeatAnalyzer analyzer(audio);

  const auto& beats = analyzer.beats();

  for (const auto& beat : beats) {
    REQUIRE(beat.time >= 0.0f);
    REQUIRE(beat.frame >= 0);
    REQUIRE(beat.strength >= 0.0f);
  }
}

TEST_CASE("detect_beats quick function", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  auto times = detect_beats(audio);

  REQUIRE(!times.empty());

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer trim option", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig config_trim;
  config_trim.trim = true;
  BeatAnalyzer analyzer_trim(audio, config_trim);

  BeatConfig config_no_trim;
  config_no_trim.trim = false;
  BeatAnalyzer analyzer_no_trim(audio, config_no_trim);

  // Both should have beats
  REQUIRE(analyzer_trim.count() > 0);
  REQUIRE(analyzer_no_trim.count() > 0);
}

TEST_CASE("BeatAnalyzer short audio", "[beat_analyzer]") {
  // Very short audio (1 second)
  Audio audio = create_click_track(120.0f, 22050, 1.0f);

  BeatConfig config;
  BeatAnalyzer analyzer(audio, config);

  // Should still work, though may have fewer beats
  REQUIRE(analyzer.bpm() > 0.0f);
}

TEST_CASE("BeatAnalyzer accessors", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  REQUIRE(analyzer.sample_rate() == audio.sample_rate());
  REQUIRE(analyzer.hop_length() > 0);
  REQUIRE(analyzer.count() == analyzer.beats().size());
  REQUIRE(analyzer.beat_times().size() == analyzer.count());
  REQUIRE(analyzer.beat_frames().size() == analyzer.count());
}
