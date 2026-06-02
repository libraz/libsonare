/// @file pitch_shift_test.cpp
/// @brief Tests for pitch shifting.

#include "effects/pitch_shift.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

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
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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

TEST_CASE("pitch_shift_ratio rejects out-of-range ratios", "[pitch_shift]") {
  // A ratio whose effective sample rate exceeds the resampler range used to be
  // silently clamped (wrong pitch); it must now throw instead.
  Audio audio = create_test_audio(440.0f, 22050, 0.25f);
  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // 22050 * 20 = 441000 Hz effective rate -> out of the supported range.
  REQUIRE_THROWS_AS(pitch_shift_ratio(audio, 20.0f, config), sonare::SonareException);
  // A moderate ratio (within ~+/-2 octaves) is still accepted.
  REQUIRE_NOTHROW(pitch_shift_ratio(audio, 2.0f, config));
}

TEST_CASE("pitch_shift native spectral backend preserves duration", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 44100, 0.25f);

  PitchShiftConfig config;
  config.backend = StretchBackend::NativeSpectral;

  Audio shifted = pitch_shift(audio, 7.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(shifted.duration(), WithinRel(audio.duration(), 0.01f));
}

TEST_CASE("pitch_shift phase vocoder backend remains available", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.5f);

  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.backend = StretchBackend::PhaseVocoder;

  Audio shifted = pitch_shift_ratio(audio, 1.0f, config);

  REQUIRE(!shifted.empty());
  REQUIRE(shifted.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(shifted.duration(), WithinRel(audio.duration(), 0.25f));
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
