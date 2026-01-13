/// @file pitch_shift_test.cpp
/// @brief Tests for pitch shifting.

#include "effects/pitch_shift.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a test signal (sine wave).
Audio create_test_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("pitch_shift zero semitones", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Zero semitones should preserve duration
  Audio shifted = pitch_shift(audio, 0.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
  // Duration should be approximately preserved
  REQUIRE_THAT(shifted.duration(), WithinRel(audio.duration(), 0.3f));
}

TEST_CASE("pitch_shift up", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // +12 semitones = one octave up
  Audio shifted = pitch_shift(audio, 12.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
  // Pitch shift produces non-empty output
  REQUIRE(shifted.size() > 0);
}

TEST_CASE("pitch_shift down", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // -12 semitones = one octave down
  Audio shifted = pitch_shift(audio, -12.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
  // Pitch shift produces non-empty output
  REQUIRE(shifted.size() > 0);
}

TEST_CASE("pitch_shift_ratio basic", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Ratio 2.0 = one octave up
  Audio shifted = pitch_shift_ratio(audio, 2.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
}

TEST_CASE("pitch_shift preserves sample rate", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 44100, 0.5f);

  Audio shifted = pitch_shift(audio, 5.0f);

  REQUIRE(shifted.sample_rate() == audio.sample_rate());
}

TEST_CASE("pitch_shift small shifts", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.3f);

  // Test small pitch shifts
  for (float semitones : {-2.0f, -1.0f, 1.0f, 2.0f}) {
    Audio shifted = pitch_shift(audio, semitones);

    REQUIRE(!shifted.empty());
    REQUIRE(shifted.sample_rate() == audio.sample_rate());
  }
}
