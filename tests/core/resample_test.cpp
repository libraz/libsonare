/// @file resample_test.cpp
/// @brief Tests for audio resampling.

#include "core/resample.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
}

float compute_rms(const float* data, size_t size) {
  float sum_sq = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    sum_sq += data[i] * data[i];
  }
  return std::sqrt(sum_sq / size);
}
}  // namespace

TEST_CASE("resample same rate returns copy", "[resample]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(1000, 440.0f, sr);

  std::vector<float> result = resample(samples.data(), samples.size(), sr, sr);

  REQUIRE(result.size() == samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    REQUIRE(result[i] == samples[i]);
  }
}

TEST_CASE("resample 44100 to 22050 (2x downsample)", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;
  constexpr float freq = 440.0f;
  constexpr int src_samples = src_sr;  // 1 second

  std::vector<float> samples = generate_sine(src_samples, freq, src_sr);
  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Output should be approximately half the length
  size_t expected_size = static_cast<size_t>(src_samples * dst_sr / src_sr);
  REQUIRE_THAT(static_cast<float>(result.size()),
               WithinRel(static_cast<float>(expected_size), 0.02f));

  // RMS should be similar (within 10% for a sine wave)
  float src_rms = compute_rms(samples.data(), samples.size());
  float dst_rms = compute_rms(result.data(), result.size());
  REQUIRE_THAT(dst_rms, WithinRel(src_rms, 0.1f));
}

TEST_CASE("resample 22050 to 44100 (2x upsample)", "[resample]") {
  constexpr int src_sr = 22050;
  constexpr int dst_sr = 44100;
  constexpr float freq = 440.0f;
  constexpr int src_samples = src_sr;  // 1 second

  std::vector<float> samples = generate_sine(src_samples, freq, src_sr);
  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Output should be approximately double the length
  size_t expected_size = static_cast<size_t>(src_samples * dst_sr / src_sr);
  REQUIRE_THAT(static_cast<float>(result.size()),
               WithinRel(static_cast<float>(expected_size), 0.02f));

  // RMS should be similar
  float src_rms = compute_rms(samples.data(), samples.size());
  float dst_rms = compute_rms(result.data(), result.size());
  REQUIRE_THAT(dst_rms, WithinRel(src_rms, 0.1f));
}

TEST_CASE("resample Audio object", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;

  std::vector<float> samples = generate_sine(src_sr, 440.0f, src_sr);  // 1 second
  Audio audio = Audio::from_vector(std::move(samples), src_sr);

  Audio resampled = resample(audio, dst_sr);

  REQUIRE(resampled.sample_rate() == dst_sr);
  // Duration should be preserved
  REQUIRE_THAT(resampled.duration(), WithinRel(audio.duration(), 0.02f));
}

TEST_CASE("resample Audio same rate returns copy", "[resample]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(1000, 440.0f, sr);
  Audio audio = Audio::from_vector(std::move(samples), sr);

  Audio resampled = resample(audio, sr);

  REQUIRE(resampled.size() == audio.size());
  REQUIRE(resampled.sample_rate() == sr);
  // Should be a copy, not the same buffer
  REQUIRE(resampled.data() != audio.data());
}

TEST_CASE("resample empty audio", "[resample]") {
  Audio empty;
  Audio result = resample(empty, 22050);
  REQUIRE(result.empty());
}

TEST_CASE("resample preserves DC offset", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;
  constexpr float dc = 0.5f;

  // DC signal with small sine
  std::vector<float> samples(src_sr);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = dc + 0.1f * std::sin(kTwoPi * 100.0f * i / src_sr);
  }

  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Compute mean of result (should be close to DC)
  float mean = std::accumulate(result.begin(), result.end(), 0.0f) / result.size();
  REQUIRE_THAT(mean, WithinRel(dc, 0.05f));
}
