/// @file audio_test.cpp
/// @brief Tests for Audio buffer class.

#include "core/audio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
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
}  // namespace

TEST_CASE("Audio from_buffer", "[audio]") {
  std::vector<float> samples = generate_sine(1000, 440.0f, 22050);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), 22050);

  REQUIRE(audio.size() == 1000);
  REQUIRE(audio.sample_rate() == 22050);
  REQUIRE(audio.channels() == 1);
  REQUIRE_FALSE(audio.empty());
  REQUIRE_THAT(audio.duration(), WithinRel(1000.0f / 22050.0f, 0.001f));
}

TEST_CASE("Audio from_vector", "[audio]") {
  std::vector<float> samples = generate_sine(2205, 440.0f, 22050);
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  REQUIRE(audio.size() == 2205);
  REQUIRE(audio.sample_rate() == 22050);
  REQUIRE_THAT(audio.duration(), WithinRel(0.1f, 0.001f));  // 100ms
}

TEST_CASE("Audio slice by time", "[audio]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(sr, 440.0f, sr);  // 1 second
  Audio audio = Audio::from_vector(std::move(samples), sr);

  SECTION("slice first half") {
    Audio slice = audio.slice(0.0f, 0.5f);
    REQUIRE(slice.size() == sr / 2);
    REQUIRE(slice.sample_rate() == sr);
    REQUIRE_THAT(slice.duration(), WithinRel(0.5f, 0.001f));
  }

  SECTION("slice second half") {
    Audio slice = audio.slice(0.5f, 1.0f);
    REQUIRE(slice.size() == sr / 2);
    // First sample should be at 0.5s mark
    REQUIRE_THAT(slice[0], WithinAbs(audio[sr / 2], 1e-6f));
  }

  SECTION("slice to end (negative end_time)") {
    Audio slice = audio.slice(0.5f);
    REQUIRE(slice.size() == sr / 2);
  }
}

TEST_CASE("Audio slice by samples", "[audio]") {
  std::vector<float> samples(1000);
  for (int i = 0; i < 1000; ++i) {
    samples[i] = static_cast<float>(i);
  }
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  SECTION("slice range") {
    Audio slice = audio.slice_samples(100, 500);
    REQUIRE(slice.size() == 400);
    REQUIRE_THAT(slice[0], WithinAbs(100.0f, 1e-6f));
    REQUIRE_THAT(slice[399], WithinAbs(499.0f, 1e-6f));
  }

  SECTION("slice shares buffer") {
    Audio slice = audio.slice_samples(0, 500);
    // Data pointers should be within the original buffer
    REQUIRE(slice.data() == audio.data());
  }
}

TEST_CASE("Audio to_mono creates copy", "[audio]") {
  std::vector<float> samples = generate_sine(1000, 440.0f, 22050);
  Audio audio = Audio::from_vector(std::move(samples), 22050);
  Audio mono = audio.to_mono();

  REQUIRE(mono.size() == audio.size());
  REQUIRE(mono.sample_rate() == audio.sample_rate());
  // Should be a copy, not sharing buffer
  REQUIRE(mono.data() != audio.data());
}

TEST_CASE("Audio empty", "[audio]") {
  Audio audio;
  REQUIRE(audio.empty());
  REQUIRE(audio.size() == 0);
  REQUIRE(audio.data() == nullptr);
  REQUIRE(audio.duration() == 0.0f);
}

TEST_CASE("Audio iterator", "[audio]") {
  std::vector<float> samples = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  float sum = 0.0f;
  for (float s : audio) {
    sum += s;
  }
  REQUIRE_THAT(sum, WithinAbs(15.0f, 1e-6f));
}
