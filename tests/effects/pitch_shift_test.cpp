/// @file pitch_shift_test.cpp
/// @brief Tests for pitch shifting.

#include "effects/pitch_shift.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
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

TEST_CASE("pitch_shift_ratio refuses only the shifted-rate floor and the int ceiling",
          "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.25f);
  PitchShiftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // 22050 * 0.04 = 882 Hz shifted rate, below the 1000 Hz floor: refused with a reason.
  try {
    (void)pitch_shift_ratio(audio, 0.04f, config);
    FAIL("expected a refusal");
  } catch (const SonareException& e) {
    REQUIRE(e.code() == ErrorCode::InvalidParameter);
    REQUIRE(std::string(e.what()).find("pitch_shift") != std::string::npos);
  }
  // 22050 * 20 = 441000 Hz is a valid rate for the resampler, so it is applied.
  REQUIRE(pitch_shift_ratio(audio, 20.0f, config).size() == audio.size());
}

TEST_CASE("pitch shift preflight checks effective rate and expansion boundaries",
          "[pitch_shift][resource_limit]") {
  PitchShiftPlan plan;

  REQUIRE(make_pitch_shift_plan(1024, 48000, 24.0f, &plan));
  REQUIRE(plan.ratio == 4.0f);
  REQUIRE(plan.effective_sample_rate == 192000);
  REQUIRE(plan.stretched_samples == 4096);
  // There is no 192 kHz ceiling: four octaves up at 48 kHz shifts through 768 kHz.
  REQUIRE(make_pitch_shift_plan(1024, 48000, 48.0f, &plan));
  REQUIRE(plan.effective_sample_rate == 768000);
  // The shifted rate must still fit the int the resampler takes.
  REQUIRE_FALSE(make_pitch_shift_ratio_plan(1024, 48000, 1.0e6f, &plan));

  // At ratio 4, 125,000,000 input samples project to the exact 500,000,000
  // sample ceiling. One more input sample must fail without materialising it.
  REQUIRE(make_pitch_shift_ratio_plan(125'000'000, 48000, 4.0f, &plan));
  REQUIRE(plan.stretched_samples == kMaxAudioBufferSize);
  REQUIRE_FALSE(make_pitch_shift_ratio_plan(125'000'001, 48000, 4.0f, &plan));
  REQUIRE(make_pitch_shift_ratio_plan(kMaxAudioBufferSize, 48000, 0.5f, &plan));
  REQUIRE_FALSE(make_pitch_shift_ratio_plan(kMaxAudioBufferSize + 1, 48000, 0.5f, &plan));
}

namespace {

/// @brief Mean period of the upward zero crossings in the middle half, as a frequency.
double middle_frequency(const Audio& audio) {
  const std::size_t begin = audio.size() / 4;
  const std::size_t end = audio.size() * 3 / 4;
  double first = -1.0;
  double last = -1.0;
  int periods = -1;
  for (std::size_t i = begin; i + 1 < end; ++i) {
    const float a = audio[i];
    const float b = audio[i + 1];
    if (a < 0.0f && b >= 0.0f) {
      const double t = static_cast<double>(i) + static_cast<double>(a / (a - b));
      if (first < 0.0) first = t;
      last = t;
      ++periods;
    }
  }
  if (periods <= 0) return 0.0;
  return static_cast<double>(audio.sample_rate()) * periods / (last - first);
}

}  // namespace

TEST_CASE("pitch_shift applies upward and downward shifts at high sample rates", "[pitch_shift]") {
  constexpr float kSourceHz = 3000.0f;
  struct Case {
    int sample_rate;
    float semitones;
  };
  for (const Case c :
       {Case{44100, 12.0f}, Case{48000, 12.0f}, Case{96000, 12.0f}, Case{96000, 13.0f},
        Case{192000, 12.0f}, Case{192000, 1.0f}, Case{384000, 12.0f}, Case{384000, -1.0f}}) {
    CAPTURE(c.sample_rate, c.semitones);
    const Audio audio = create_test_audio(kSourceHz, c.sample_rate, 0.25f);
    const Audio shifted = pitch_shift(audio, c.semitones);
    REQUIRE(shifted.size() == audio.size());
    REQUIRE(shifted.sample_rate() == c.sample_rate);
    const double expected =
        kSourceHz *
        std::pow(2.0, c.semitones / static_cast<double>(constants::kSemitonesPerOctave));
    REQUIRE_THAT(middle_frequency(shifted), WithinRel(expected, 0.01));
  }
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

TEST_CASE("pitch_shift rejects a hop below the half-window overlap contract", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.25f);

  for (StretchBackend backend : {StretchBackend::NativeSpectral, StretchBackend::PhaseVocoder}) {
    PitchShiftConfig sparse;
    sparse.n_fft = 1024;
    sparse.hop_length = 1024;
    sparse.backend = backend;
    REQUIRE_THROWS_AS(pitch_shift(audio, 2.0f, sparse), SonareException);
    REQUIRE_THROWS_AS(pitch_shift_ratio(audio, 1.1f, sparse), SonareException);

    PitchShiftConfig ok = sparse;
    ok.hop_length = 256;
    REQUIRE_NOTHROW(pitch_shift(audio, 2.0f, ok));
  }
}

TEST_CASE("pitch_shift accepts an even n_fft that is not a power of two", "[pitch_shift]") {
  Audio audio = create_test_audio(440.0f, 22050, 0.25f);

  for (StretchBackend backend : {StretchBackend::NativeSpectral, StretchBackend::PhaseVocoder}) {
    PitchShiftConfig config;
    config.n_fft = 1500;
    config.hop_length = 250;
    config.backend = backend;

    Audio shifted = pitch_shift(audio, 2.0f, config);
    REQUIRE(shifted.sample_rate() == audio.sample_rate());
    REQUIRE(!shifted.empty());
    for (const float value : shifted) REQUIRE(std::isfinite(value));
  }
}

namespace {

void require_exact_length(float seconds, int sample_rate) {
  const Audio audio = create_test_audio(440.0f, sample_rate, seconds);
  for (const StretchBackend backend :
       {StretchBackend::NativeSpectral, StretchBackend::PhaseVocoder}) {
    for (const float ratio : {std::pow(2.0f, 7.0f / 12.0f), std::pow(2.0f, -5.0f / 12.0f), 1.37f}) {
      CAPTURE(seconds, sample_rate, static_cast<int>(backend), ratio);
      PitchShiftConfig config;
      config.backend = backend;
      REQUIRE(pitch_shift_ratio(audio, ratio, config).size() == audio.size());
    }
  }
}

}  // namespace

TEST_CASE("pitch_shift returns exactly the input's sample count on both backends",
          "[pitch_shift]") {
  require_exact_length(1.0f, 8000);
  require_exact_length(1.0f, 48000);
}

TEST_CASE("pitch_shift keeps the exact sample count over long inputs", "[pitch_shift][.][slow]") {
  for (const float seconds : {10.0f, 60.0f}) {
    require_exact_length(seconds, 8000);
    require_exact_length(seconds, 48000);
  }
}
