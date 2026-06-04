/// @file pitch_test.cpp
/// @brief Tests for pitch detection (YIN and pYIN).

#include "feature/pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::test::generate_sine;

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
    phase += 2.0f * sonare::constants::kPiD * freq / sr;
  }
  return Audio::from_vector(std::move(samples), sr);
}

std::vector<float> naive_yin_difference(const std::vector<float>& frame, int max_lag) {
  std::vector<float> diff(static_cast<size_t>(max_lag), 0.0f);
  const int window = static_cast<int>(frame.size()) / 2;
  for (int tau = 0; tau < max_lag; ++tau) {
    float sum = 0.0f;
    for (int j = 0; j < window; ++j) {
      const float delta = frame[static_cast<size_t>(j)] - frame[static_cast<size_t>(j + tau)];
      sum += delta * delta;
    }
    diff[static_cast<size_t>(tau)] = sum;
  }
  return diff;
}

}  // namespace

TEST_CASE("yin_difference basic", "[pitch]") {
  // Generate a 440 Hz sine wave at 22050 Hz
  // Period = 22050 / 440 = ~50 samples
  std::vector<float> frame(2048);
  float freq = 440.0f;
  int sr = 22050;
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = std::sin(2.0f * sonare::constants::kPiD * freq * i / sr);
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

TEST_CASE("yin_difference matches naive constant-window definition", "[pitch]") {
  std::mt19937 rng(1337);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  std::vector<float> frame(2048);
  for (auto& sample : frame) {
    sample = dist(rng);
  }

  const auto expected = naive_yin_difference(frame, 512);
  const auto actual = yin_difference(frame.data(), static_cast<int>(frame.size()), 512);

  REQUIRE(actual.size() == expected.size());
  for (size_t index = 0; index < actual.size(); ++index) {
    INFO("lag: " << index);
    REQUIRE_THAT(actual[index], WithinAbs(expected[index], 1.0e-3f));
  }
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

TEST_CASE("yin 440Hz constant window accuracy", "[pitch]") {
  // With the corrected constant window size (frame_length / 2) in the YIN
  // difference function, pitch detection should be more accurate.
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  float freq = yin(audio.data(), 2048, 22050, 100.0f, 1000.0f, 0.2f);

  REQUIRE(freq > 0.0f);
  // Tighter tolerance: within 1% of 440 Hz
  REQUIRE_THAT(freq, WithinRel(440.0f, 0.01f));
}

TEST_CASE("yin_with_confidence", "[pitch]") {
  Audio audio = generate_sine(440.0f, 0.2f, 22050);

  float confidence;
  float freq = yin_with_confidence(audio.data(), 2048, 22050, 100.0f, 1000.0f, 0.2f, &confidence);

  REQUIRE(freq > 0.0f);
  REQUIRE(confidence > 0.5f);  // Should have good confidence for clean sine
  REQUIRE(confidence <= 1.0f);
}

TEST_CASE("yin_find_pitch honors the configured voicing threshold", "[pitch]") {
  const std::vector<float> cmndf = {1.0f, 0.9f, 0.7f, 0.4f, 0.45f, 0.8f};

  REQUIRE(yin_find_pitch(cmndf, 0.3f, 1, 6) == 0.0f);
  REQUIRE_THAT(yin_find_pitch(cmndf, 0.5f, 1, 6), WithinAbs(3.36f, 0.01f));
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

TEST_CASE("pyin - 5 second 440Hz tone (flat-vector Viterbi regression)", "[pitch]") {
  // P1-13 regression test: ensure the flat-vector Viterbi implementation still
  // tracks a long stationary tone correctly. Five seconds at hop=512, sr=22050
  // yields >200 frames, exercising the row-major observation/viterbi/backtrack
  // layout across many transitions.
  Audio audio = generate_sine(440.0f, 5.0f, 22050);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.3f;

  PitchResult result = pyin(audio, config);

  REQUIRE(result.n_frames() > 100);

  // Median is robust to occasional edge / unvoiced frames.
  float median_f0 = result.median_f0();
  REQUIRE_THAT(median_f0, WithinRel(440.0f, 0.01f));

  // Vast majority of frames should be voiced and within 2% of 440 Hz.
  int voiced_in_band = 0;
  int voiced_total = 0;
  for (int i = 0; i < result.n_frames(); ++i) {
    if (result.voiced_flag[i]) {
      ++voiced_total;
      const float f = result.f0[i];
      if (f > 0.0f && std::abs(f - 440.0f) / 440.0f < 0.02f) {
        ++voiced_in_band;
      }
    }
  }
  REQUIRE(voiced_total > result.n_frames() * 8 / 10);
  REQUIRE(voiced_in_band > voiced_total * 9 / 10);
}

TEST_CASE("pyin - stepped pitch tracking (Viterbi follows discontinuities)", "[pitch]") {
  // P1-13 regression test: a synthetic signal with two abrupt pitch steps
  // (220 Hz -> 330 Hz -> 440 Hz, ~1 second each). The Viterbi pass must
  // follow each step within a small transition window. This stresses the
  // backtrack traversal across the flat backtrack[n_frames * n_states] array.
  const int sr = 22050;
  const float seg_dur = 1.0f;
  const std::vector<float> segs = {220.0f, 330.0f, 440.0f};

  const int seg_samples = static_cast<int>(sr * seg_dur);
  std::vector<float> samples(static_cast<size_t>(seg_samples) * segs.size());
  float phase = 0.0f;
  for (size_t s = 0; s < segs.size(); ++s) {
    const float freq = segs[s];
    for (int i = 0; i < seg_samples; ++i) {
      samples[s * seg_samples + i] = std::sin(phase);
      phase += 2.0f * sonare::constants::kPiD * freq / sr;
    }
  }
  Audio audio = Audio::from_vector(std::move(samples), sr);

  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.3f;

  PitchResult result = pyin(audio, config);
  REQUIRE(result.n_frames() > 30);

  const int n = result.n_frames();
  // Sample mid-third of each segment to avoid step transition frames.
  const auto seg_mean = [&](float start_t, float end_t) {
    const int begin = static_cast<int>(start_t * n);
    const int end = static_cast<int>(end_t * n);
    float sum = 0.0f;
    int count = 0;
    for (int i = begin; i < end; ++i) {
      if (result.voiced_flag[i] && result.f0[i] > 0.0f) {
        sum += result.f0[i];
        ++count;
      }
    }
    REQUIRE(count > 0);
    return sum / count;
  };

  const float mean1 = seg_mean(0.05f, 0.28f);
  const float mean2 = seg_mean(0.38f, 0.62f);
  const float mean3 = seg_mean(0.72f, 0.95f);

  REQUIRE_THAT(mean1, WithinRel(220.0f, 0.03f));
  REQUIRE_THAT(mean2, WithinRel(330.0f, 0.03f));
  REQUIRE_THAT(mean3, WithinRel(440.0f, 0.03f));

  // Viterbi must follow the step ordering monotonically across segments.
  REQUIRE(mean1 < mean2);
  REQUIRE(mean2 < mean3);
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

TEST_CASE("pyin rejects degenerate fmin == fmax", "[pitch][edge]") {
  Audio audio = generate_sine(440.0f, 0.5f, 22050);

  PitchConfig config;
  config.fmin = 440.0f;
  config.fmax = 440.0f;  // n_pitch_bins would be degenerate (log2(1) == 0)

  REQUIRE_THROWS(pyin(audio, config));

  // fmax below fmin is equally invalid.
  config.fmax = 400.0f;
  REQUIRE_THROWS(pyin(audio, config));
}

TEST_CASE("pyin caps max_period at frame_length/2 for reliable lags", "[pitch][edge]") {
  // Regression for max_period = frame_length - 1, which admitted unreliable
  // high-lag troughs computed from only a handful of sample products. With a low
  // fmin the requested max_period (sr/fmin) falls in (frame_length/2,
  // frame_length-1], so the tighter frame_length/2 cap is what now bounds it.
  // A clean 220 Hz tone must still track accurately under the cap.
  Audio audio = generate_sine(220.0f, 0.5f, 22050);

  PitchConfig config;
  config.frame_length = 2048;
  config.fmin = 15.0f;  // sr/fmin = 1470, between frame_length/2 (1024) and 2047
  config.fmax = 1000.0f;
  config.threshold = 0.3f;

  PitchResult result = pyin(audio, config);
  REQUIRE(result.n_frames() > 0);

  // The tracked pitch should still resolve near 220 Hz despite the low fmin.
  float mean_f0 = result.mean_f0();
  REQUIRE_THAT(mean_f0, WithinRel(220.0f, 0.05f));
}

TEST_CASE("pitch_tuning returns the librosa bin left edge", "[pitch]") {
  // A single frequency ~0.3 of a semitone sharp of A4 has residual frac ~= 0.3.
  // With resolution=0.01 (n_bins=100) the measured residual lands in the bin
  // covering 0.30, and librosa returns that bin's LEFT edge
  // (np.linspace(-0.5, 0.5, 100, endpoint=False)), i.e. 0.29 or 0.30 depending
  // on which side of the bin boundary the residual falls — NOT the bin center
  // 0.305 that the pre-fix code returned. Accept the adjacent left-edge bins
  // (half-bin tolerance) but exclude the old center convention.
  const float freq = sonare::constants::kA4Hz * std::pow(2.0f, 0.3f / 12.0f);
  const float tuning = pitch_tuning({freq}, 0.01f, 12);
  REQUIRE_THAT(tuning, WithinAbs(0.295f, 0.0075f));

  // An exactly in-tune A4 (residual 0) lands in bin 50; left edge is 0.0.
  const float in_tune = pitch_tuning({sonare::constants::kA4Hz}, 0.01f, 12);
  REQUIRE_THAT(in_tune, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("estimate_tuning uses a global magnitude median", "[pitch]") {
  // Build an uneven-energy signal: a loud in-tune A4 segment followed by a quiet
  // detuned segment. librosa thresholds piptrack peaks against ONE global median
  // over all positive-pitch magnitudes, which suppresses the quiet detuned peaks,
  // so the estimate should track the loud in-tune content (~0 tuning) rather than
  // being pulled toward the quiet detuned segment as a per-frame median would.
  const int sr = 22050;
  const float seg = 1.0f;
  const int n_seg = static_cast<int>(sr * seg);
  std::vector<float> samples;
  samples.reserve(static_cast<size_t>(2) * n_seg);

  const float a4 = sonare::constants::kA4Hz;
  const float detuned = a4 * std::pow(2.0f, 0.4f / 12.0f);  // 0.4 semitone sharp
  for (int i = 0; i < n_seg; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples.push_back(0.9f * std::sin(2.0f * sonare::constants::kPiD * a4 * t));
  }
  for (int i = 0; i < n_seg; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples.push_back(0.02f * std::sin(2.0f * sonare::constants::kPiD * detuned * t));
  }
  Audio audio = Audio::from_vector(std::move(samples), sr);

  const float tuning = estimate_tuning(audio);
  // The loud in-tune segment dominates the global-median-thresholded peaks.
  REQUIRE_THAT(tuning, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("estimate_tuning handles even-sized peak magnitude medians", "[pitch]") {
  const int sr = 22050;
  const int n_samples = 4096;
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[static_cast<size_t>(i)] =
        0.7f * std::sin(2.0f * sonare::constants::kPiD * sonare::constants::kA4Hz * t);
  }
  Audio audio = Audio::from_vector(std::move(samples), sr);

  const float tuning = estimate_tuning(audio, 2048, 2048);
  REQUIRE_THAT(tuning, WithinAbs(0.0f, 0.1f));
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
