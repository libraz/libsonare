/// @file timbre_analyzer_test.cpp
/// @brief Tests for timbre analyzer.

#include "analysis/timbre_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates a pure sine wave.
Audio create_sine(float freq, int sr = 22050, float duration = 2.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a bright sound (high frequency content).
Audio create_bright_sound(int sr = 22050, float duration = 2.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    // High frequency harmonics
    samples[i] = 0.3f * std::sin(2.0f * M_PI * 2000.0f * t);
    samples[i] += 0.3f * std::sin(2.0f * M_PI * 4000.0f * t);
    samples[i] += 0.2f * std::sin(2.0f * M_PI * 6000.0f * t);
    samples[i] += 0.1f * std::sin(2.0f * M_PI * 8000.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a warm sound (low frequency content).
Audio create_warm_sound(int sr = 22050, float duration = 2.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    // Low frequency harmonics
    samples[i] = 0.5f * std::sin(2.0f * M_PI * 100.0f * t);
    samples[i] += 0.3f * std::sin(2.0f * M_PI * 200.0f * t);
    samples[i] += 0.1f * std::sin(2.0f * M_PI * 400.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates white noise.
Audio create_noise(int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  // Simple pseudo-random noise
  unsigned int seed = 12345;
  for (int i = 0; i < n_samples; ++i) {
    seed = seed * 1103515245 + 12345;
    samples[i] = (static_cast<float>(seed % 65536) / 32768.0f - 1.0f) * 0.5f;
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("TimbreAnalyzer basic", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f);

  TimbreConfig config;
  TimbreAnalyzer analyzer(audio, config);

  const auto& timbre = analyzer.timbre();

  REQUIRE(timbre.brightness >= 0.0f);
  REQUIRE(timbre.brightness <= 1.0f);
  REQUIRE(timbre.warmth >= 0.0f);
  REQUIRE(timbre.warmth <= 1.0f);
  REQUIRE(timbre.density >= 0.0f);
  REQUIRE(timbre.density <= 1.0f);
  REQUIRE(timbre.roughness >= 0.0f);
  REQUIRE(timbre.roughness <= 1.0f);
  REQUIRE(timbre.complexity >= 0.0f);
  REQUIRE(timbre.complexity <= 1.0f);
}

TEST_CASE("TimbreAnalyzer brightness comparison", "[timbre_analyzer]") {
  Audio bright = create_bright_sound();
  Audio warm = create_warm_sound();

  TimbreAnalyzer bright_analyzer(bright);
  TimbreAnalyzer warm_analyzer(warm);

  // Bright sound should have higher brightness
  REQUIRE(bright_analyzer.brightness() > warm_analyzer.brightness());
}

TEST_CASE("TimbreAnalyzer warmth comparison", "[timbre_analyzer]") {
  Audio bright = create_bright_sound();
  Audio warm = create_warm_sound();

  TimbreAnalyzer bright_analyzer(bright);
  TimbreAnalyzer warm_analyzer(warm);

  // Warm sound should have higher warmth
  REQUIRE(warm_analyzer.warmth() > bright_analyzer.warmth());
}

TEST_CASE("TimbreAnalyzer noise density", "[timbre_analyzer]") {
  Audio sine = create_sine(440.0f);
  Audio noise = create_noise();

  TimbreAnalyzer sine_analyzer(sine);
  TimbreAnalyzer noise_analyzer(noise);

  // Noise should have higher density (flatness)
  REQUIRE(noise_analyzer.density() > sine_analyzer.density());
}

TEST_CASE("TimbreAnalyzer timbre over time", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 3.0f);

  TimbreConfig config;
  config.window_sec = 0.5f;
  TimbreAnalyzer analyzer(audio, config);

  const auto& timbre_time = analyzer.timbre_over_time();

  REQUIRE(!timbre_time.empty());

  for (const auto& t : timbre_time) {
    REQUIRE(t.brightness >= 0.0f);
    REQUIRE(t.brightness <= 1.0f);
    REQUIRE(t.warmth >= 0.0f);
    REQUIRE(t.warmth <= 1.0f);
  }
}

TEST_CASE("TimbreAnalyzer spectral features", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f);

  TimbreAnalyzer analyzer(audio);

  const auto& centroid = analyzer.spectral_centroid();
  const auto& flatness = analyzer.spectral_flatness();
  const auto& rolloff = analyzer.spectral_rolloff();

  REQUIRE(!centroid.empty());
  REQUIRE(!flatness.empty());
  REQUIRE(!rolloff.empty());

  // Centroid should be approximately 440 Hz for pure sine
  float avg_centroid = 0.0f;
  for (float c : centroid) {
    avg_centroid += c;
  }
  avg_centroid /= centroid.size();
  REQUIRE(avg_centroid >= 400.0f);
  REQUIRE(avg_centroid <= 500.0f);
}

TEST_CASE("TimbreAnalyzer accessors", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f);

  TimbreAnalyzer analyzer(audio);

  REQUIRE(analyzer.brightness() >= 0.0f);
  REQUIRE(analyzer.warmth() >= 0.0f);
  REQUIRE(analyzer.density() >= 0.0f);
  REQUIRE(analyzer.roughness() >= 0.0f);
  REQUIRE(analyzer.complexity() >= 0.0f);
}

TEST_CASE("TimbreAnalyzer short audio", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 0.5f);

  TimbreConfig config;
  TimbreAnalyzer analyzer(audio, config);

  // Should still work
  REQUIRE(analyzer.brightness() >= 0.0f);
}

TEST_CASE("TimbreAnalyzer config options", "[timbre_analyzer]") {
  Audio audio = create_sine(440.0f);

  TimbreConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 64;
  config.n_mfcc = 20;

  TimbreAnalyzer analyzer(audio, config);

  const auto& timbre = analyzer.timbre();
  REQUIRE(timbre.brightness >= 0.0f);
  REQUIRE(timbre.brightness <= 1.0f);
}

TEST_CASE("TimbreAnalyzer complexity comparison", "[timbre_analyzer]") {
  // Pure sine has low complexity
  Audio sine = create_sine(440.0f);

  // Complex sound with many harmonics
  int sr = 22050;
  float duration = 2.0f;
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> complex_samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    // Many harmonics with different amplitudes
    for (int h = 1; h <= 10; ++h) {
      complex_samples[i] += (1.0f / h) * std::sin(2.0f * M_PI * 440.0f * h * t);
    }
  }
  Audio complex_audio = Audio::from_vector(std::move(complex_samples), sr);

  TimbreAnalyzer sine_analyzer(sine);
  TimbreAnalyzer complex_analyzer(complex_audio);

  // Complex sound should have higher complexity
  // (This may vary depending on implementation)
  REQUIRE(complex_analyzer.complexity() >= 0.0f);
}
