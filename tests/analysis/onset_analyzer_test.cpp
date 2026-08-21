/// @file onset_analyzer_test.cpp
/// @brief Tests for onset analyzer.

#include "analysis/onset_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "feature/onset.h"
#include "util/constants.h"

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
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freq * t);
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

TEST_CASE("OnsetAnalyzer backtrack lands on the preceding local minimum", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetDetectConfig config_no_bt;
  config_no_bt.backtrack = false;

  OnsetDetectConfig config_bt;
  config_bt.backtrack = true;
  // Large enough not to bound the search, so this case tests the rule alone.
  config_bt.backtrack_range = 100000;

  OnsetAnalyzer analyzer_no_bt(audio, config_no_bt);
  OnsetAnalyzer analyzer_bt(audio, config_bt);

  REQUIRE(analyzer_no_bt.count() > 0);
  REQUIRE(analyzer_bt.count() == analyzer_no_bt.count());

  const std::vector<float>& envelope = analyzer_no_bt.onset_strength();
  // The envelope this analyzer backtracks over is mean-removed, so most of it
  // is negative; the stopping rule must not depend on the sign.
  const bool has_negative =
      std::any_of(envelope.begin(), envelope.end(), [](float v) { return v < 0.0f; });
  REQUIRE(has_negative);

  const std::vector<int> detected = analyzer_no_bt.onset_frames();
  const std::vector<int> backtracked = analyzer_bt.onset_frames();
  for (size_t i = 0; i < detected.size(); ++i) {
    // Backtracking only ever moves an onset earlier.
    REQUIRE(backtracked[i] <= detected[i]);
    // And it stops at a local minimum, in librosa's sense: the curve is
    // non-increasing INTO the landing frame and strictly rising OUT of it.
    // The first half is not strict -- a plateau's last sample is a valid
    // landing point, and requiring envelope[m-1] > envelope[m] would describe
    // the plateau's FIRST sample instead, which is the rule this project used
    // to implement and which disagrees with librosa on every flat run.
    const int frame = backtracked[i];
    if (frame > 0) {
      const size_t m = static_cast<size_t>(frame);
      REQUIRE(envelope[m - 1] >= envelope[m]);
      if (m + 1 < envelope.size()) {
        REQUIRE(envelope[m] < envelope[m + 1]);
      }
    }
  }
}

TEST_CASE("OnsetAnalyzer backtrack agrees with the shared onset_backtrack", "[onset_analyzer]") {
  // One backtracking rule: the analyzer's backtrack option and the standalone
  // onset_backtrack primitive must return the same frames for the same envelope.
  Audio audio = create_click_track(4);

  OnsetDetectConfig config_no_bt;
  OnsetDetectConfig config_bt;
  config_bt.backtrack = true;
  config_bt.backtrack_range = 100000;  // Unbounded in practice; see the bound case below.

  OnsetAnalyzer analyzer_no_bt(audio, config_no_bt);
  OnsetAnalyzer analyzer_bt(audio, config_bt);
  REQUIRE(analyzer_no_bt.count() > 0);

  const std::vector<int> detected = analyzer_no_bt.onset_frames();
  const std::vector<int> expected = onset_backtrack(detected, analyzer_no_bt.onset_strength());
  REQUIRE(analyzer_bt.onset_frames() == expected);
}

TEST_CASE("OnsetAnalyzer backtrack_range bounds how far an onset travels", "[onset_analyzer]") {
  Audio audio = create_click_track(4);

  OnsetDetectConfig config_no_bt;
  OnsetAnalyzer analyzer_no_bt(audio, config_no_bt);
  REQUIRE(analyzer_no_bt.count() > 0);

  const std::vector<int> detected = analyzer_no_bt.onset_frames();
  const std::vector<int> unbounded = onset_backtrack(detected, analyzer_no_bt.onset_strength());

  // Derive the bound from the material instead of hard-coding it, so the check
  // cannot go vacuous when the backtracking rule changes how far an onset
  // travels. One frame short of the longest unbounded travel guarantees the
  // bound clamps at least one onset.
  int longest_travel = 0;
  for (size_t i = 0; i < detected.size(); ++i) {
    longest_travel = std::max(longest_travel, detected[i] - unbounded[i]);
  }
  REQUIRE(longest_travel > 0);
  const int kRange = longest_travel - 1;

  OnsetDetectConfig config_bt;
  config_bt.backtrack = true;
  config_bt.backtrack_range = kRange;
  OnsetAnalyzer analyzer_bt(audio, config_bt);
  const std::vector<int> bounded = analyzer_bt.onset_frames();

  bool bound_binds = false;
  for (size_t i = 0; i < detected.size(); ++i) {
    if (unbounded[i] < detected[i] - kRange) bound_binds = true;
  }
  REQUIRE(bound_binds);

  for (size_t i = 0; i < detected.size(); ++i) {
    REQUIRE(detected[i] - bounded[i] <= kRange);
    REQUIRE(bounded[i] == std::max(unbounded[i], detected[i] - kRange));
  }
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

TEST_CASE("OnsetAnalyzer detects a flat-topped peak", "[onset_analyzer]") {
  // A plateau of equal maxima used to be rejected entirely: every frame on the
  // plateau had an equal neighbour, so the old `>= current` test marked none of
  // them a local maximum. Peak picking must now accept the first plateau frame.
  std::vector<float> onset_strength(60, 0.0f);
  // A 3-frame flat top at frames 20, 21, 22 (all equal), with a rising edge.
  onset_strength[19] = 0.5f;
  onset_strength[20] = 1.0f;
  onset_strength[21] = 1.0f;
  onset_strength[22] = 1.0f;
  onset_strength[23] = 0.4f;

  OnsetDetectConfig config;
  config.pre_max = 1;
  config.post_max = 1;
  config.pre_avg = 1;
  config.post_avg = 1;
  config.wait = 1;
  config.delta = 0.0f;
  config.threshold = 0.5f;

  OnsetAnalyzer analyzer(onset_strength, 22050, 512, config);
  auto frames = analyzer.onset_frames();

  REQUIRE(frames.size() == 1);
  // The first frame of the plateau is the detected onset (left edge).
  REQUIRE(frames.front() == 20);
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
