/// @file normalize_test.cpp
/// @brief Tests for audio normalization and trimming.

#include "effects/normalize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a test signal with specified amplitude.
Audio create_audio_with_amplitude(float amplitude, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(2.0f * M_PI * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with silence at beginning and end.
Audio create_audio_with_silence(int sr = 22050) {
  std::vector<float> samples;

  // 0.2s silence
  for (int i = 0; i < sr / 5; ++i) {
    samples.push_back(0.0f);
  }

  // 0.5s tone
  for (int i = 0; i < sr / 2; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples.push_back(0.5f * std::sin(2.0f * M_PI * 440.0f * t));
  }

  // 0.3s silence
  for (int i = 0; i < sr * 3 / 10; ++i) {
    samples.push_back(0.0f);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("peak_db", "[normalize]") {
  Audio half_amplitude = create_audio_with_amplitude(0.5f);
  Audio full_amplitude = create_audio_with_amplitude(1.0f);

  float half_peak = peak_db(half_amplitude);
  float full_peak = peak_db(full_amplitude);

  // Full amplitude should be 0 dB
  REQUIRE_THAT(full_peak, WithinAbs(0.0f, 0.1f));

  // Half amplitude should be -6 dB
  REQUIRE_THAT(half_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("rms_db", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  float rms = rms_db(audio);

  // RMS of sine at amplitude 1 is 1/sqrt(2) ≈ -3 dB
  REQUIRE_THAT(rms, WithinAbs(-3.0f, 0.5f));
}

TEST_CASE("apply_gain", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.5f);

  // Apply +6 dB gain (double amplitude)
  Audio gained = apply_gain(audio, 6.0f);

  float original_peak = peak_db(audio);
  float gained_peak = peak_db(gained);

  REQUIRE_THAT(gained_peak - original_peak, WithinAbs(6.0f, 0.5f));
}

TEST_CASE("apply_gain negative", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  // Apply -6 dB gain (halve amplitude)
  Audio gained = apply_gain(audio, -6.0f);

  float gained_peak = peak_db(gained);

  REQUIRE_THAT(gained_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("normalize to 0 dB", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.25f);

  Audio normalized = normalize(audio, 0.0f);

  float normalized_peak = peak_db(normalized);

  REQUIRE_THAT(normalized_peak, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("normalize to -6 dB", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  Audio normalized = normalize(audio, -6.0f);

  float normalized_peak = peak_db(normalized);

  REQUIRE_THAT(normalized_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("normalize_rms", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.25f);

  Audio normalized = normalize_rms(audio, -10.0f);

  float normalized_rms = rms_db(normalized);

  REQUIRE_THAT(normalized_rms, WithinAbs(-10.0f, 0.5f));
}

TEST_CASE("trim silence", "[normalize]") {
  Audio audio = create_audio_with_silence();

  Audio trimmed = trim(audio, -40.0f);

  // Trimmed should be shorter
  REQUIRE(trimmed.size() < audio.size());

  // Trimmed should not be empty
  REQUIRE(!trimmed.empty());
}

TEST_CASE("detect_silence_boundaries", "[normalize]") {
  Audio audio = create_audio_with_silence();

  auto [start, end] = detect_silence_boundaries(audio, -40.0f);

  // Start should be after initial silence
  REQUIRE(start > 0);

  // End should be before final silence
  REQUIRE(end < audio.size());

  // Valid range
  REQUIRE(start < end);
}

TEST_CASE("fade_in", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f, 22050, 1.0f);

  Audio faded = fade_in(audio, 0.1f);

  REQUIRE(!faded.empty());
  REQUIRE(faded.size() == audio.size());

  // First sample should be near zero
  REQUIRE(std::abs(faded.data()[0]) < 0.01f);

  // Sample at 50% of fade should be between 0 and original
  int mid_fade = audio.sample_rate() / 20;  // 0.05s
  REQUIRE(std::abs(faded.data()[mid_fade]) < std::abs(audio.data()[mid_fade]));
}

TEST_CASE("fade_out", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f, 22050, 1.0f);

  Audio faded = fade_out(audio, 0.1f);

  REQUIRE(!faded.empty());
  REQUIRE(faded.size() == audio.size());

  // Last sample should be near zero
  REQUIRE(std::abs(faded.data()[faded.size() - 1]) < 0.01f);
}

TEST_CASE("fade preserves duration", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  Audio faded_in = fade_in(audio, 0.1f);
  Audio faded_out = fade_out(audio, 0.1f);

  REQUIRE(faded_in.duration() == audio.duration());
  REQUIRE(faded_out.duration() == audio.duration());
}
