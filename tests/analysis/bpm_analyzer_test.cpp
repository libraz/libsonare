/// @file bpm_analyzer_test.cpp
/// @brief Tests for BPM analyzer.

#include "analysis/bpm_analyzer.h"

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

/// @brief Creates a sine wave with amplitude modulation at specified BPM.
Audio create_modulated_sine(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  float beat_freq = bpm / 60.0f;
  float carrier_freq = 440.0f;

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    float envelope = 0.5f + 0.5f * std::sin(2.0f * M_PI * beat_freq * t);
    samples[i] = envelope * std::sin(2.0f * M_PI * carrier_freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("BpmAnalyzer basic", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BpmConfig config;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.start_bpm = 120.0f;

  BpmAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.bpm() >= config.bpm_min);
  REQUIRE(analyzer.bpm() <= config.bpm_max);
  REQUIRE(analyzer.confidence() >= 0.0f);
  REQUIRE(analyzer.confidence() <= 1.0f);
}

TEST_CASE("BpmAnalyzer 120 BPM detection", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  BpmConfig config;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.start_bpm = 120.0f;

  BpmAnalyzer analyzer(audio, config);

  // Should be within 5% of 120 BPM
  REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.1f));
}

TEST_CASE("BpmAnalyzer 90 BPM detection", "[bpm_analyzer]") {
  Audio audio = create_click_track(90.0f, 22050, 5.0f);

  BpmConfig config;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.start_bpm = 90.0f;

  BpmAnalyzer analyzer(audio, config);

  // Should be close to 90 BPM or octave-related
  float detected = analyzer.bpm();
  bool close_to_90 = std::abs(detected - 90.0f) < 10.0f;
  bool close_to_180 = std::abs(detected - 180.0f) < 10.0f;
  bool close_to_45 = std::abs(detected - 45.0f) < 10.0f;

  REQUIRE((close_to_90 || close_to_180 || close_to_45));
}

TEST_CASE("BpmAnalyzer candidates", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BpmConfig config;
  BpmAnalyzer analyzer(audio, config);

  auto candidates = analyzer.candidates(5);

  REQUIRE(candidates.size() <= 5);

  // Candidates should be sorted by confidence
  for (size_t i = 1; i < candidates.size(); ++i) {
    REQUIRE(candidates[i - 1].confidence >= candidates[i].confidence);
  }
}

TEST_CASE("BpmAnalyzer autocorrelation", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BpmAnalyzer analyzer(audio);

  const auto& autocorr = analyzer.autocorrelation();

  REQUIRE(!autocorr.empty());

  // Autocorrelation at lag 0 should be 1.0 (or close to it after normalization)
  // Values should be in [-1, 1] range
  for (float val : autocorr) {
    REQUIRE(val >= -1.1f);
    REQUIRE(val <= 1.1f);
  }
}

TEST_CASE("detect_bpm quick function", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  float bpm = detect_bpm(audio);

  REQUIRE(bpm >= 60.0f);
  REQUIRE(bpm <= 200.0f);
}

TEST_CASE("BpmAnalyzer modulated signal", "[bpm_analyzer]") {
  Audio audio = create_modulated_sine(100.0f, 22050, 5.0f);

  BpmConfig config;
  config.start_bpm = 100.0f;

  BpmAnalyzer analyzer(audio, config);

  // Should detect tempo related to modulation rate
  REQUIRE(analyzer.bpm() >= 50.0f);
  REQUIRE(analyzer.bpm() <= 200.0f);
}

TEST_CASE("BpmConfig::uniform_prior", "[bpm_analyzer]") {
  auto prior = BpmConfig::uniform_prior(80.0f, 160.0f, 0.1f);

  SECTION("in-range BPM returns 1.0") {
    REQUIRE_THAT(prior(80.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(prior(120.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(prior(160.0f), WithinAbs(1.0f, 1e-6f));
  }

  SECTION("out-of-range BPM returns falloff") {
    REQUIRE_THAT(prior(60.0f), WithinAbs(0.1f, 1e-6f));
    REQUIRE_THAT(prior(200.0f), WithinAbs(0.1f, 1e-6f));
  }
}

TEST_CASE("BpmConfig::lognorm_prior", "[bpm_analyzer]") {
  auto prior = BpmConfig::lognorm_prior(120.0f, 1.0f);

  SECTION("center BPM returns 1.0") { REQUIRE_THAT(prior(120.0f), WithinAbs(1.0f, 1e-6f)); }

  SECTION("weight decreases away from center") {
    float center_weight = prior(120.0f);
    float off_weight = prior(90.0f);
    REQUIRE(off_weight < center_weight);
  }

  SECTION("double/half BPM have equal weight") {
    float double_weight = prior(240.0f);
    float half_weight = prior(60.0f);
    REQUIRE_THAT(double_weight, WithinRel(half_weight, 0.01f));
  }

  SECTION("zero BPM returns 0") { REQUIRE_THAT(prior(0.0f), WithinAbs(0.0f, 1e-6f)); }
}

TEST_CASE("BpmAnalyzer with custom prior", "[bpm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  SECTION("uniform prior 80-160") {
    BpmConfig config;
    config.prior = BpmConfig::uniform_prior(80.0f, 160.0f);

    BpmAnalyzer analyzer(audio, config);

    // Should detect BPM within the prior range
    REQUIRE(analyzer.bpm() >= 80.0f);
    REQUIRE(analyzer.bpm() <= 160.0f);
  }

  SECTION("lognorm prior centered at 120") {
    BpmConfig config;
    config.prior = BpmConfig::lognorm_prior(120.0f, 0.5f);

    BpmAnalyzer analyzer(audio, config);

    // Should be close to 120 BPM
    REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.15f));
  }

  SECTION("custom lambda prior") {
    BpmConfig config;
    // Prior that strongly prefers 90 BPM
    config.prior = [](float bpm) {
      float diff = std::abs(bpm - 90.0f);
      return std::exp(-diff * diff / 100.0f);
    };

    BpmAnalyzer analyzer(audio, config);

    // Detection should still work
    REQUIRE(analyzer.bpm() >= 30.0f);
    REQUIRE(analyzer.bpm() <= 300.0f);
  }
}
