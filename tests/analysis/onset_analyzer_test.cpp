/// @file onset_analyzer_test.cpp
/// @brief Tests for onset analyzer.

#include "analysis/onset_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a click track with transients.
Audio create_click_track(int n_clicks, int sr = 22050, float duration = 2.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float interval = duration / static_cast<float>(n_clicks);
  int click_length = sr / 100;  // 10ms click

  for (int c = 0; c < n_clicks; ++c) {
    int start = static_cast<int>(c * interval * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a steady tone (no onsets).
Audio create_steady_tone(float freq = 440.0f, int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("OnsetAnalyzer basic", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetDetectConfig config;
  OnsetAnalyzer analyzer(audio, config);

  // Should detect some onsets
  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("OnsetAnalyzer click count", "[onset_analyzer]") {
  int expected_clicks = 5;
  Audio audio = create_click_track(expected_clicks, 22050, 3.0f);

  OnsetDetectConfig config;
  config.pre_max = 2;
  config.post_max = 2;
  config.delta = 0.05f;

  OnsetAnalyzer analyzer(audio, config);

  // Should detect approximately the expected number of clicks
  int detected = static_cast<int>(analyzer.count());
  REQUIRE(detected >= expected_clicks - 2);
  REQUIRE(detected <= expected_clicks + 2);
}

TEST_CASE("OnsetAnalyzer onset times", "[onset_analyzer]") {
  Audio audio = create_click_track(4, 22050, 2.0f);

  OnsetAnalyzer analyzer(audio);

  auto times = analyzer.onset_times();

  // Times should be sorted
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }

  // Times should be within audio duration
  for (float t : times) {
    REQUIRE(t >= 0.0f);
    REQUIRE(t <= 2.0f);
  }
}

TEST_CASE("OnsetAnalyzer onset frames", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetAnalyzer analyzer(audio);

  auto frames = analyzer.onset_frames();
  auto times = analyzer.onset_times();

  REQUIRE(frames.size() == times.size());

  // Frames should be non-negative
  for (int f : frames) {
    REQUIRE(f >= 0);
  }
}

TEST_CASE("OnsetAnalyzer onset strength", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetAnalyzer analyzer(audio);

  const auto& strength = analyzer.onset_strength();

  REQUIRE(!strength.empty());

  // Strength values should be finite
  for (float s : strength) {
    REQUIRE(std::isfinite(s));
  }
}

TEST_CASE("OnsetAnalyzer steady tone few onsets", "[onset_analyzer]") {
  Audio audio = create_steady_tone(440.0f, 22050, 2.0f);

  OnsetDetectConfig config;
  config.delta = 0.2f;  // Higher threshold to reduce false positives

  OnsetAnalyzer analyzer(audio, config);

  // Steady tone should have fewer onsets than a click track
  // May detect a few due to edge effects
  REQUIRE(analyzer.count() <= 5);
}

TEST_CASE("OnsetAnalyzer backtrack", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetDetectConfig config_no_bt;
  config_no_bt.backtrack = false;

  OnsetDetectConfig config_bt;
  config_bt.backtrack = true;
  config_bt.backtrack_range = 5;

  OnsetAnalyzer analyzer_no_bt(audio, config_no_bt);
  OnsetAnalyzer analyzer_bt(audio, config_bt);

  // Both should detect onsets
  REQUIRE(analyzer_no_bt.count() > 0);
  REQUIRE(analyzer_bt.count() > 0);

  // Backtracked times might be slightly earlier
  // (but not guaranteed, depends on signal)
}

TEST_CASE("OnsetAnalyzer sample rate and hop length", "[onset_analyzer]") {
  Audio audio = create_click_track(4, 44100, 2.0f);

  OnsetDetectConfig config;
  config.hop_length = 256;

  OnsetAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.sample_rate() == 44100);
  REQUIRE(analyzer.hop_length() == 256);
}

TEST_CASE("detect_onsets quick function", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  auto times = detect_onsets(audio);

  REQUIRE(!times.empty());

  // Times should be sorted
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("OnsetAnalyzer wait parameter", "[onset_analyzer]") {
  // Create onset strength envelope with consecutive peaks
  std::vector<float> onset_strength(100, 0.0f);
  // Create peaks at frames 10, 12, 15, 30, 32, 50
  onset_strength[10] = 1.0f;
  onset_strength[12] = 0.9f;  // Close to frame 10
  onset_strength[15] = 0.8f;  // Close to frame 12
  onset_strength[30] = 1.0f;
  onset_strength[32] = 0.85f;  // Close to frame 30
  onset_strength[50] = 1.0f;

  int sr = 22050;
  int hop_length = 512;

  SECTION("wait = 0 allows consecutive detections") {
    OnsetDetectConfig config;
    config.pre_max = 1;
    config.post_max = 1;
    config.pre_avg = 1;
    config.post_avg = 1;
    config.wait = 0;
    config.delta = 0.0f;
    config.threshold = 0.5f;

    OnsetAnalyzer analyzer(onset_strength, sr, hop_length, config);
    auto frames = analyzer.onset_frames();

    // With wait=0, should detect more peaks
    REQUIRE(frames.size() >= 4);
  }

  SECTION("wait = 5 prevents close detections") {
    OnsetDetectConfig config;
    config.pre_max = 1;
    config.post_max = 1;
    config.pre_avg = 1;
    config.post_avg = 1;
    config.wait = 5;
    config.delta = 0.0f;
    config.threshold = 0.5f;

    OnsetAnalyzer analyzer(onset_strength, sr, hop_length, config);
    auto frames = analyzer.onset_frames();

    // With wait=5, consecutive detections must be > 5 frames apart
    for (size_t i = 1; i < frames.size(); ++i) {
      REQUIRE(frames[i] - frames[i - 1] > config.wait);
    }
  }

  SECTION("wait = 10 further reduces detections") {
    OnsetDetectConfig config;
    config.pre_max = 1;
    config.post_max = 1;
    config.pre_avg = 1;
    config.post_avg = 1;
    config.wait = 10;
    config.delta = 0.0f;
    config.threshold = 0.5f;

    OnsetAnalyzer analyzer(onset_strength, sr, hop_length, config);
    auto frames = analyzer.onset_frames();

    // With wait=10, should detect fewer peaks (frames 10, 30, 50 at most)
    for (size_t i = 1; i < frames.size(); ++i) {
      REQUIRE(frames[i] - frames[i - 1] > config.wait);
    }
  }
}
