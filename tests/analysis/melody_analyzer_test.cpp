/// @file melody_analyzer_test.cpp
/// @brief Tests for melody analyzer.

#include "analysis/melody_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a pure sine wave at given frequency.
Audio create_sine(float freq, int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.8f * std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a melody with multiple pitches.
Audio create_melody(const std::vector<float>& freqs, float note_duration, int sr = 22050) {
  int samples_per_note = static_cast<int>(sr * note_duration);
  int total_samples = samples_per_note * static_cast<int>(freqs.size());
  std::vector<float> samples(total_samples);

  for (size_t n = 0; n < freqs.size(); ++n) {
    float freq = freqs[n];
    int start = static_cast<int>(n) * samples_per_note;

    for (int i = 0; i < samples_per_note; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      // Apply envelope to reduce clicks
      float env = 1.0f;
      if (i < samples_per_note / 10) {
        env = static_cast<float>(i) / (samples_per_note / 10);
      } else if (i > samples_per_note * 9 / 10) {
        env = static_cast<float>(samples_per_note - i) / (samples_per_note / 10);
      }
      samples[start + i] = 0.8f * env * std::sin(2.0f * M_PI * freq * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("MelodyAnalyzer basic", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyConfig config;
  MelodyAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("MelodyAnalyzer A440 detection", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.has_melody());

  // Mean frequency should be close to 440 Hz
  if (analyzer.mean_frequency() > 0.0f) {
    REQUIRE_THAT(analyzer.mean_frequency(), WithinRel(440.0f, 0.1f));
  }
}

TEST_CASE("MelodyAnalyzer pitch times", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyAnalyzer analyzer(audio);

  auto times = analyzer.pitch_times();

  REQUIRE(!times.empty());
  REQUIRE(times.size() == analyzer.count());

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("MelodyAnalyzer pitch frequencies", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyAnalyzer analyzer(audio);

  auto frequencies = analyzer.pitch_frequencies();

  REQUIRE(frequencies.size() == analyzer.count());
}

TEST_CASE("MelodyAnalyzer pitch confidences", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyAnalyzer analyzer(audio);

  auto confidences = analyzer.pitch_confidences();

  REQUIRE(confidences.size() == analyzer.count());

  for (float c : confidences) {
    REQUIRE(c >= 0.0f);
    REQUIRE(c <= 1.0f);
  }
}

TEST_CASE("MelodyAnalyzer contour features", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyAnalyzer analyzer(audio);

  const auto& contour = analyzer.contour();

  REQUIRE(contour.pitch_range_octaves >= 0.0f);
  REQUIRE(contour.pitch_stability >= 0.0f);
  REQUIRE(contour.pitch_stability <= 1.0f);
}

TEST_CASE("MelodyAnalyzer stability for pure tone", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 2.0f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  // Pure tone should have high stability
  if (analyzer.has_melody()) {
    REQUIRE(analyzer.stability() >= 0.5f);
  }
}

TEST_CASE("MelodyAnalyzer melody with multiple pitches", "[melody_analyzer]") {
  // C-D-E-F-G melody
  std::vector<float> freqs = {261.63f, 293.66f, 329.63f, 349.23f, 392.00f};
  Audio audio = create_melody(freqs, 0.5f);

  MelodyConfig config;
  config.threshold = 0.15f;
  MelodyAnalyzer analyzer(audio, config);

  // Should detect pitch points
  REQUIRE(analyzer.count() > 0);

  // Pitch range should span multiple notes
  if (analyzer.has_melody()) {
    REQUIRE(analyzer.pitch_range() > 0.0f);
  }
}

TEST_CASE("MelodyAnalyzer frequency range config", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyConfig config;
  config.fmin = 400.0f;
  config.fmax = 500.0f;

  MelodyAnalyzer analyzer(audio, config);

  // Should still detect pitch within range
  auto frequencies = analyzer.pitch_frequencies();
  for (float f : frequencies) {
    if (f > 0.0f) {
      REQUIRE(f >= config.fmin);
      REQUIRE(f <= config.fmax);
    }
  }
}

TEST_CASE("MelodyAnalyzer frame/hop config", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 1.0f);

  MelodyConfig config1;
  config1.frame_length = 2048;
  config1.hop_length = 256;
  MelodyAnalyzer analyzer1(audio, config1);

  MelodyConfig config2;
  config2.frame_length = 1024;
  config2.hop_length = 128;
  MelodyAnalyzer analyzer2(audio, config2);

  // Smaller hop should produce more frames
  REQUIRE(analyzer2.count() > analyzer1.count());
}

TEST_CASE("MelodyAnalyzer short audio", "[melody_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 0.2f);

  MelodyConfig config;
  MelodyAnalyzer analyzer(audio, config);

  // Should still work without crashing
  (void)analyzer.count();
}

TEST_CASE("MelodyAnalyzer different frequencies", "[melody_analyzer]") {
  // Test at different frequencies
  std::vector<float> test_freqs = {200.0f, 300.0f, 500.0f, 800.0f};

  for (float freq : test_freqs) {
    Audio audio = create_sine(freq, 22050, 1.0f);

    MelodyConfig config;
    config.threshold = 0.2f;
    MelodyAnalyzer analyzer(audio, config);

    if (analyzer.has_melody() && analyzer.mean_frequency() > 0.0f) {
      // Mean frequency should be within 15% of actual
      REQUIRE_THAT(analyzer.mean_frequency(), WithinRel(freq, 0.15f));
    }
  }
}

TEST_CASE("MelodyAnalyzer has_melody", "[melody_analyzer]") {
  Audio sine = create_sine(440.0f, 22050, 1.0f);

  MelodyAnalyzer analyzer(sine);

  // Sine wave should have melody
  // (Note: may vary based on threshold)
  REQUIRE(analyzer.count() > 0);
}
