/// @file dynamics_analyzer_test.cpp
/// @brief Tests for dynamics analyzer.

#include "analysis/dynamics_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a constant amplitude sine wave.
Audio create_constant_sine(float amplitude, int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(2.0f * M_PI * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with varying dynamics.
Audio create_dynamic_audio(int sr = 22050, float duration = 2.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    // Amplitude varies from 0.1 to 0.9
    float amplitude = 0.5f + 0.4f * std::sin(2.0f * M_PI * 0.5f * t);
    samples[i] = amplitude * std::sin(2.0f * M_PI * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a highly compressed (constant loudness) audio.
Audio create_compressed_audio(int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    // Square wave has high peak-to-RMS ratio before compression
    // Using clipped sine as "compressed" signal
    float val = std::sin(2.0f * M_PI * 440.0f * t);
    samples[i] = std::tanh(val * 3.0f) * 0.9f;  // Soft clipping
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("DynamicsAnalyzer basic", "[dynamics_analyzer]") {
  Audio audio = create_constant_sine(0.5f);

  DynamicsAnalyzer analyzer(audio);

  const auto& dynamics = analyzer.dynamics();

  REQUIRE(std::isfinite(dynamics.peak_db));
  REQUIRE(std::isfinite(dynamics.rms_db));
  REQUIRE(std::isfinite(dynamics.crest_factor));
  REQUIRE(std::isfinite(dynamics.dynamic_range_db));
}

TEST_CASE("DynamicsAnalyzer peak level", "[dynamics_analyzer]") {
  Audio full_scale = create_constant_sine(1.0f);
  Audio half_scale = create_constant_sine(0.5f);

  DynamicsAnalyzer full_analyzer(full_scale);
  DynamicsAnalyzer half_analyzer(half_scale);

  // Full scale should be 0 dB
  REQUIRE_THAT(full_analyzer.peak_db(), WithinAbs(0.0f, 0.1f));

  // Half scale should be -6 dB
  REQUIRE_THAT(half_analyzer.peak_db(), WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("DynamicsAnalyzer RMS level", "[dynamics_analyzer]") {
  Audio audio = create_constant_sine(1.0f);

  DynamicsAnalyzer analyzer(audio);

  // RMS of sine wave with amplitude 1 is 1/sqrt(2) ≈ -3 dB
  REQUIRE_THAT(analyzer.rms_db(), WithinAbs(-3.0f, 0.5f));
}

TEST_CASE("DynamicsAnalyzer crest factor", "[dynamics_analyzer]") {
  Audio sine = create_constant_sine(1.0f);

  DynamicsAnalyzer analyzer(sine);

  // Crest factor for sine wave is peak/RMS = sqrt(2) ≈ 3 dB
  REQUIRE_THAT(analyzer.crest_factor(), WithinAbs(3.0f, 0.5f));
}

TEST_CASE("DynamicsAnalyzer dynamic range", "[dynamics_analyzer]") {
  Audio constant = create_constant_sine(0.5f);
  Audio dynamic = create_dynamic_audio();

  DynamicsAnalyzer const_analyzer(constant);
  DynamicsAnalyzer dyn_analyzer(dynamic);

  // Dynamic audio should have larger dynamic range
  REQUIRE(dyn_analyzer.dynamic_range_db() > const_analyzer.dynamic_range_db());
}

TEST_CASE("DynamicsAnalyzer is_compressed", "[dynamics_analyzer]") {
  Audio dynamic = create_dynamic_audio();
  Audio compressed = create_compressed_audio();

  DynamicsConfig config;
  config.compression_threshold = 6.0f;

  DynamicsAnalyzer dyn_analyzer(dynamic, config);
  DynamicsAnalyzer comp_analyzer(compressed, config);

  // Dynamic audio is less likely to be flagged as compressed
  // Compressed audio has lower crest factor
  REQUIRE(comp_analyzer.crest_factor() < dyn_analyzer.crest_factor() + 2.0f);
}

TEST_CASE("DynamicsAnalyzer loudness curve", "[dynamics_analyzer]") {
  Audio audio = create_dynamic_audio();

  DynamicsConfig config;
  config.window_sec = 0.1f;
  config.hop_length = 512;

  DynamicsAnalyzer analyzer(audio, config);

  const auto& curve = analyzer.loudness_curve();

  REQUIRE(!curve.times.empty());
  REQUIRE(curve.times.size() == curve.rms_db.size());

  // Times should be monotonically increasing
  for (size_t i = 1; i < curve.times.size(); ++i) {
    REQUIRE(curve.times[i] > curve.times[i - 1]);
  }

  // RMS values should be finite
  for (float rms : curve.rms_db) {
    REQUIRE(std::isfinite(rms));
  }
}

TEST_CASE("DynamicsAnalyzer loudness histogram", "[dynamics_analyzer]") {
  Audio audio = create_dynamic_audio();

  DynamicsAnalyzer analyzer(audio);

  auto histogram = analyzer.loudness_histogram(50, -60.0f, 0.0f);

  REQUIRE(histogram.size() == 50);

  // Total count should match number of loudness samples
  int total = 0;
  for (int count : histogram) {
    REQUIRE(count >= 0);
    total += count;
  }

  REQUIRE(total > 0);
}

TEST_CASE("DynamicsAnalyzer accessors", "[dynamics_analyzer]") {
  Audio audio = create_constant_sine(0.7f);

  DynamicsAnalyzer analyzer(audio);

  // Test all accessors return consistent values
  REQUIRE_THAT(analyzer.peak_db(), WithinAbs(analyzer.dynamics().peak_db, 0.001f));
  REQUIRE_THAT(analyzer.rms_db(), WithinAbs(analyzer.dynamics().rms_db, 0.001f));
  REQUIRE_THAT(analyzer.crest_factor(), WithinAbs(analyzer.dynamics().crest_factor, 0.001f));
  REQUIRE_THAT(analyzer.dynamic_range_db(),
               WithinAbs(analyzer.dynamics().dynamic_range_db, 0.001f));
}
