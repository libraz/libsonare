/// @file pitch_test.cpp
/// @brief Tests for pitch detection (YIN and pYIN).

#include "feature/pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Generates a pure sine wave.
Audio generate_sine(float freq, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Generates a sawtooth wave (rich harmonics).
Audio generate_sawtooth(float freq, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);
  float period = static_cast<float>(sr) / freq;
  for (int i = 0; i < n_samples; ++i) {
    float phase = std::fmod(static_cast<float>(i), period) / period;
    samples[i] = 2.0f * phase - 1.0f;
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Generates audio with pitch sweep.
Audio generate_sweep(float freq_start, float freq_end, float duration, int sr = 22050) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);
  float phase = 0.0f;
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / n_samples;
    float freq = freq_start + (freq_end - freq_start) * t;
    samples[i] = std::sin(phase);
    phase += 2.0f * M_PI * freq / sr;
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("yin_difference basic", "[pitch]") {
  // Generate a 440 Hz sine wave at 22050 Hz
  // Period = 22050 / 440 = ~50 samples
  std::vector<float> frame(2048);
  float freq = 440.0f;
  int sr = 22050;
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = std::sin(2.0f * M_PI * freq * i / sr);
  }

  int expected_period = sr / static_cast<int>(freq);  // ~50
  auto diff = yin_difference(frame.data(), frame.size(), 512);

  REQUIRE(diff.size() == 512);
  // d(0) should be 0
  REQUIRE_THAT(diff[0], WithinAbs(0.0f, 1e-6f));
  // d(tau) should have minimum near the period
  // Check that the minimum is in the expected range
  float min_val = diff[expected_period];
  REQUIRE(min_val < diff[expected_period / 2]);
}

TEST_CASE("yin_cmndf normalization", "[pitch]") {
  std::vector<float> diff = {0.0f, 1.0f, 2.0f, 3.0f, 2.0f, 1.0f};
  auto cmndf = yin_cmndf(diff);

  REQUIRE(cmndf.size() == diff.size());
  REQUIRE_THAT(cmndf[0], WithinAbs(1.0f, 1e-6f));
  // CMNDF values should be normalized
  for (size_t i = 1; i < cmndf.size(); ++i) {
    REQUIRE(cmndf[i] >= 0.0f);
  }
}

TEST_CASE("yin single frame - 440 Hz sine", "[pitch]") {
  Audio audio = generate_sine(440.0f, 0.2f, 22050);

  float freq = yin(audio.data(), 2048, 22050, 100.0f, 1000.0f, 0.2f);

  REQUIRE(freq > 0.0f);
  REQUIRE_THAT(freq, WithinRel(440.0f, 0.02f));  // Within 2%
}

TEST_CASE("yin single frame - 220 Hz sine", "[pitch]") {
  Audio audio = generate_sine(220.0f, 0.2f, 22050);

  float freq = yin(audio.data(), 2048, 22050, 100.0f, 500.0f, 0.2f);

  REQUIRE(freq > 0.0f);
  REQUIRE_THAT(freq, WithinRel(220.0f, 0.02f));
}

TEST_CASE("yin single frame - sawtooth", "[pitch]") {
  Audio audio = generate_sawtooth(330.0f, 0.2f, 22050);

  float freq = yin(audio.data(), 2048, 22050, 100.0f, 1000.0f, 0.3f);

  // Should detect fundamental despite harmonics
  REQUIRE(freq > 0.0f);
  REQUIRE_THAT(freq, WithinRel(330.0f, 0.05f));
}

TEST_CASE("yin_with_confidence", "[pitch]") {
  Audio audio = generate_sine(440.0f, 0.2f, 22050);

  float confidence;
  float freq = yin_with_confidence(audio.data(), 2048, 22050, 100.0f, 1000.0f, 0.2f, &confidence);

  REQUIRE(freq > 0.0f);
  REQUIRE(confidence > 0.5f);  // Should have good confidence for clean sine
  REQUIRE(confidence <= 1.0f);
}

TEST_CASE("yin_track - constant pitch", "[pitch]") {
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.2f;

  PitchResult result = yin_track(audio, config);

  REQUIRE(result.n_frames() > 0);

  // Most frames should detect ~440 Hz
  int voiced_count = 0;
  float freq_sum = 0.0f;
  for (int i = 0; i < result.n_frames(); ++i) {
    if (result.voiced_flag[i]) {
      ++voiced_count;
      freq_sum += result.f0[i];
    }
  }

  REQUIRE(voiced_count > result.n_frames() / 2);
  float mean_freq = freq_sum / voiced_count;
  REQUIRE_THAT(mean_freq, WithinRel(440.0f, 0.02f));
}

TEST_CASE("pyin - constant pitch", "[pitch]") {
  Audio audio = generate_sine(440.0f, 1.0f, 22050);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.3f;

  PitchResult result = pyin(audio, config);

  REQUIRE(result.n_frames() > 0);

  // pYIN should give smoother results
  float mean_f0 = result.mean_f0();
  REQUIRE_THAT(mean_f0, WithinRel(440.0f, 0.02f));
}

TEST_CASE("pyin - pitch sweep", "[pitch]") {
  Audio audio = generate_sweep(220.0f, 440.0f, 2.0f, 22050);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.3f;

  PitchResult result = pyin(audio, config);

  REQUIRE(result.n_frames() > 10);

  // First frames should be near 220 Hz, last near 440 Hz
  int n = result.n_frames();

  // Average of first 10% of frames
  float first_sum = 0.0f;
  int first_count = 0;
  for (int i = 0; i < n / 10; ++i) {
    if (result.voiced_flag[i]) {
      first_sum += result.f0[i];
      ++first_count;
    }
  }

  // Average of last 10% of frames
  float last_sum = 0.0f;
  int last_count = 0;
  for (int i = n - n / 10; i < n; ++i) {
    if (result.voiced_flag[i]) {
      last_sum += result.f0[i];
      ++last_count;
    }
  }

  if (first_count > 0 && last_count > 0) {
    float first_avg = first_sum / first_count;
    float last_avg = last_sum / last_count;

    REQUIRE(first_avg < last_avg);  // Pitch should increase
    REQUIRE_THAT(first_avg, WithinRel(220.0f, 0.1f));
    REQUIRE_THAT(last_avg, WithinRel(440.0f, 0.1f));
  }
}

TEST_CASE("pyin - sawtooth wave", "[pitch]") {
  Audio audio = generate_sawtooth(330.0f, 1.0f, 22050);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;

  PitchResult result = pyin(audio, config);

  // Should detect fundamental frequency
  float mean_f0 = result.mean_f0();
  REQUIRE_THAT(mean_f0, WithinRel(330.0f, 0.05f));
}

TEST_CASE("freq_to_midi conversion", "[pitch]") {
  REQUIRE_THAT(freq_to_midi(440.0f), WithinAbs(69.0f, 0.01f));  // A4
  REQUIRE_THAT(freq_to_midi(261.63f), WithinAbs(60.0f, 0.1f));  // C4 (middle C)
  REQUIRE_THAT(freq_to_midi(880.0f), WithinAbs(81.0f, 0.01f));  // A5
  REQUIRE_THAT(freq_to_midi(220.0f), WithinAbs(57.0f, 0.01f));  // A3
}

TEST_CASE("midi_to_freq conversion", "[pitch]") {
  REQUIRE_THAT(midi_to_freq(69.0f), WithinAbs(440.0f, 0.01f));  // A4
  REQUIRE_THAT(midi_to_freq(60.0f), WithinAbs(261.63f, 0.1f));  // C4
  REQUIRE_THAT(midi_to_freq(81.0f), WithinAbs(880.0f, 0.01f));  // A5
}

TEST_CASE("freq_to_midi and midi_to_freq roundtrip", "[pitch]") {
  std::vector<float> freqs = {220.0f, 330.0f, 440.0f, 550.0f, 660.0f, 880.0f};

  for (float freq : freqs) {
    float midi = freq_to_midi(freq);
    float back = midi_to_freq(midi);
    REQUIRE_THAT(back, WithinRel(freq, 0.001f));
  }
}

TEST_CASE("PitchResult statistics", "[pitch]") {
  PitchResult result;
  result.f0 = {440.0f, 0.0f, 445.0f, 435.0f, 0.0f};
  result.voiced_flag = {true, false, true, true, false};
  result.voiced_prob = {0.9f, 0.1f, 0.85f, 0.88f, 0.05f};

  float median = result.median_f0();
  float mean = result.mean_f0();

  // Mean of 440, 445, 435 = 440
  REQUIRE_THAT(mean, WithinAbs(440.0f, 0.1f));

  // Median of 435, 440, 445 = 440
  REQUIRE_THAT(median, WithinAbs(440.0f, 0.1f));
}

TEST_CASE("pyin empty audio", "[pitch]") {
  Audio audio;

  // Empty audio should throw an exception
  REQUIRE_THROWS(pyin(audio, PitchConfig()));
}

TEST_CASE("yin_track with fill_na", "[pitch]") {
  // Generate audio with some silence
  std::vector<float> samples(22050, 0.0f);  // 1 second silence
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  PitchConfig config;
  config.fill_na = true;

  PitchResult result = yin_track(audio, config);

  REQUIRE(result.n_frames() > 0);

  // All frames should be unvoiced with f0 = 0
  for (int i = 0; i < result.n_frames(); ++i) {
    REQUIRE(result.f0[i] == 0.0f);
    REQUIRE_FALSE(result.voiced_flag[i]);
  }
}
