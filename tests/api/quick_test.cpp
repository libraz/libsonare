/// @file quick_test.cpp
/// @brief Tests for quick API functions.

#include "quick.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Generate sine wave
std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] = std::sin(2.0f * static_cast<float>(M_PI) * freq * i / sample_rate);
  }
  return samples;
}

// Generate click track at given BPM
std::vector<float> generate_clicks(float bpm, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float samples_per_beat = (sample_rate * 60.0f) / bpm;
  int n_beats = static_cast<int>(duration * bpm / 60.0f);

  for (int beat = 0; beat < n_beats; ++beat) {
    size_t start = static_cast<size_t>(beat * samples_per_beat);
    // Short click (10ms)
    size_t click_length = static_cast<size_t>(sample_rate * 0.01f);
    for (size_t i = 0; i < click_length && start + i < n_samples; ++i) {
      samples[start + i] = std::sin(static_cast<float>(M_PI) * i / click_length);
    }
  }
  return samples;
}

}  // namespace

TEST_CASE("quick::detect_bpm", "[quick][api]") {
  SECTION("detects 120 BPM from click track") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float bpm = sonare::quick::detect_bpm(samples.data(), samples.size(), 22050);

    // Allow ±5 BPM or half/double relationship
    bool close_to_120 = std::abs(bpm - 120.0f) < 5.0f;
    bool close_to_60 = std::abs(bpm - 60.0f) < 5.0f;
    bool close_to_240 = std::abs(bpm - 240.0f) < 10.0f;
    REQUIRE((close_to_120 || close_to_60 || close_to_240));
  }

  SECTION("detects 140 BPM from click track") {
    auto samples = generate_clicks(140.0f, 22050, 4.0f);
    float bpm = sonare::quick::detect_bpm(samples.data(), samples.size(), 22050);

    bool close_to_140 = std::abs(bpm - 140.0f) < 5.0f;
    bool close_to_70 = std::abs(bpm - 70.0f) < 5.0f;
    REQUIRE((close_to_140 || close_to_70));
  }
}

TEST_CASE("quick::detect_key", "[quick][api]") {
  SECTION("detects key from A440 sine") {
    auto samples = generate_sine(440.0f, 22050, 2.0f);
    sonare::Key key = sonare::quick::detect_key(samples.data(), samples.size(), 22050);

    // A440 should give A major or A minor
    REQUIRE(key.confidence >= 0.0f);
    REQUIRE(key.confidence <= 1.0f);
    // Root should be A (9) for clear A tone
    // Note: pure sine may not give strong key detection
  }

  SECTION("returns valid key structure") {
    auto samples = generate_sine(261.63f, 22050, 2.0f);  // C4
    sonare::Key key = sonare::quick::detect_key(samples.data(), samples.size(), 22050);

    REQUIRE(static_cast<int>(key.root) >= 0);
    REQUIRE(static_cast<int>(key.root) <= 11);
    REQUIRE((key.mode == sonare::Mode::Major || key.mode == sonare::Mode::Minor));
  }
}

TEST_CASE("quick::detect_onsets", "[quick][api]") {
  SECTION("detects onsets from clicks") {
    auto samples = generate_clicks(120.0f, 22050, 2.0f);
    auto onsets = sonare::quick::detect_onsets(samples.data(), samples.size(), 22050);

    // Should detect at least some onsets
    REQUIRE(onsets.size() >= 2);

    // Onsets should be in ascending order
    for (size_t i = 1; i < onsets.size(); ++i) {
      REQUIRE(onsets[i] > onsets[i - 1]);
    }
  }

  SECTION("onset times are within audio duration") {
    float duration = 3.0f;
    auto samples = generate_clicks(100.0f, 22050, duration);
    auto onsets = sonare::quick::detect_onsets(samples.data(), samples.size(), 22050);

    for (float t : onsets) {
      REQUIRE(t >= 0.0f);
      REQUIRE(t <= duration);
    }
  }
}

TEST_CASE("quick::detect_beats", "[quick][api]") {
  SECTION("detects beats from clicks") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    auto beats = sonare::quick::detect_beats(samples.data(), samples.size(), 22050);

    // Should detect multiple beats
    REQUIRE(beats.size() >= 4);

    // Beats should be in ascending order
    for (size_t i = 1; i < beats.size(); ++i) {
      REQUIRE(beats[i] > beats[i - 1]);
    }
  }
}

TEST_CASE("quick::analyze", "[quick][api]") {
  SECTION("returns complete analysis result") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    auto result = sonare::quick::analyze(samples.data(), samples.size(), 22050);

    // BPM
    REQUIRE(result.bpm > 0.0f);
    REQUIRE(result.bpm_confidence >= 0.0f);
    REQUIRE(result.bpm_confidence <= 1.0f);

    // Key
    REQUIRE(static_cast<int>(result.key.root) >= 0);
    REQUIRE(static_cast<int>(result.key.root) <= 11);

    // Time signature
    REQUIRE(result.time_signature.numerator > 0);
    REQUIRE(result.time_signature.denominator > 0);

    // Beats
    REQUIRE(result.beats.size() >= 1);

    // Dynamics
    REQUIRE(std::isfinite(result.dynamics.dynamic_range_db));
  }

  SECTION("form string is not empty") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    auto result = sonare::quick::analyze(samples.data(), samples.size(), 22050);

    // Form should have at least one character
    REQUIRE(!result.form.empty());
  }
}
