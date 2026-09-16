/// @file pitch_test.cpp
/// @brief Tests for pitch detection (YIN and pYIN).

#include "feature/pitch.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

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
  double acf_zero = 0.0;
  for (float sample : frame) {
    acf_zero += static_cast<double>(sample) * sample;
  }
  double prefix_square = 0.0;
  for (int tau = 0; tau < max_lag && tau < static_cast<int>(frame.size()); ++tau) {
    double acf = 0.0;
    for (int j = 0; j + tau < static_cast<int>(frame.size()); ++j) {
      acf +=
          static_cast<double>(frame[static_cast<size_t>(j)]) * frame[static_cast<size_t>(j + tau)];
    }
    if (tau > 0) {
      const float sample = frame[static_cast<size_t>(tau - 1)];
      prefix_square += static_cast<double>(sample) * sample;
    }
    diff[static_cast<size_t>(tau)] =
        static_cast<float>(std::max(0.0, 2.0 * (acf_zero - acf) - prefix_square));
  }
  return diff;
}

/// @brief piptrack's parabolic interpolation, copied from the implementation.
float oracle_parabolic_interp(float ym1, float y0, float yp1) {
  float denom = ym1 - 2.0f * y0 + yp1;
  if (std::abs(denom) < sonare::constants::kEpsilon) {
    return 0.0f;
  }
  return 0.5f * (ym1 - yp1) / denom;
}

/// @brief piptrack's peak scan as a frame-major column walk.
/// @details The traversal the implementation used before it was rewritten bin-major: one
///          column pass for the per-frame maximum and a second for the local-peak scan,
///          both striding by n_frames. Kept as an independent reference so a change of
///          traversal has to reproduce the old result exactly.
PiptrackResult oracle_piptrack(const Audio& audio, int n_fft, int hop_length, float fmin,
                               float fmax, float threshold) {
  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop_length;
  cfg.win_length = n_fft;
  cfg.center = true;
  const Spectrogram spec = Spectrogram::compute(audio, cfg);

  const std::vector<float>& mag = spec.magnitude();
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const int sr = audio.sample_rate();

  std::vector<float> bin_freq(static_cast<size_t>(n_bins));
  for (int k = 0; k < n_bins; ++k) {
    bin_freq[static_cast<size_t>(k)] =
        static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(n_fft);
  }

  PiptrackResult out;
  out.n_bins = n_bins;
  out.n_frames = n_frames;
  out.pitches.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  out.magnitudes.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);

  for (int t = 0; t < n_frames; ++t) {
    float maxm = 0.0f;
    for (int k = 0; k < n_bins; ++k) maxm = std::max(maxm, mag[k * n_frames + t]);
    const float gate = threshold * maxm;

    for (int k = 1; k < n_bins; ++k) {
      if (bin_freq[k] < fmin || bin_freq[k] > fmax) continue;
      const bool at_top_edge = (k == n_bins - 1);
      float a = mag[(k - 1) * n_frames + t];
      float b = mag[k * n_frames + t];
      float c = at_top_edge ? b : mag[(k + 1) * n_frames + t];
      if (b <= a || b < c || b < gate) continue;
      float shift = at_top_edge ? 0.0f : oracle_parabolic_interp(a, b, c);
      float freq =
          (static_cast<float>(k) + shift) * static_cast<float>(sr) / static_cast<float>(n_fft);
      out.pitches[k * n_frames + t] = freq;
      float peak_mag = b;
      if (!at_top_edge) {
        float denom = a - 2.0f * b + c;
        if (std::abs(denom) > sonare::constants::kEpsilon) {
          peak_mag = b - 0.25f * (a - c) * shift;
        }
      }
      out.magnitudes[k * n_frames + t] = peak_mag;
    }
  }
  return out;
}

/// @brief A signal whose spectrum reaches every branch of the piptrack peak scan.
/// @details Four regions: a harmonic stack (many in-band local maxima), exact silence longer
///          than one frame (every bin of those frames is equal, so the scan's comparisons are
///          decided by equality), a two-tone region, and a Nyquist alternation (a peak in the
///          topmost bin, where the fabricated right neighbour equals the centre exactly).
Audio piptrack_traversal_fixture(int sr, size_t n_samples) {
  std::vector<float> samples(n_samples, 0.0f);
  const size_t silence_begin = n_samples / 4;
  const size_t silence_end = silence_begin + n_samples / 8;
  const size_t nyquist_begin = n_samples * 5 / 8;
  uint32_t state = 2463534242u;
  for (size_t i = 0; i < n_samples; ++i) {
    if (i >= silence_begin && i < silence_end) continue;
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    if (i < silence_begin) {
      samples[i] = 0.6f * std::sin(sonare::constants::kTwoPi * 250.0f * t) +
                   0.3f * std::sin(sonare::constants::kTwoPi * 750.0f * t) +
                   0.15f * std::sin(sonare::constants::kTwoPi * 1750.0f * t);
    } else if (i < nyquist_begin) {
      samples[i] = 0.4f * std::sin(sonare::constants::kTwoPi * 440.0f * t) +
                   0.4f * std::sin(sonare::constants::kTwoPi * 1180.0f * t);
    } else {
      samples[i] = (i % 2 == 0) ? 0.5f : -0.5f;
    }
    // A deterministic noise floor so the spectrum carries local maxima between the partials
    // rather than a handful of clean peaks.
    state = state * 1664525u + 1013904223u;
    const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    samples[i] += 0.01f * (unit - 0.5f);
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

TEST_CASE("yin_difference matches librosa shrinking-window definition", "[pitch]") {
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

TEST_CASE("A frame carrying a non-finite sample does not report certainty", "[pitch]") {
  // Walked stage by stage rather than end to end: a zero confidence arrives by
  // several routes, and only the stage counts say which one ran.
  Audio audio = generate_sine(440.0f, 0.2f, 22050);
  std::vector<float> frame(audio.data(), audio.data() + 2048);
  frame[1000] = std::numeric_limits<float>::quiet_NaN();

  const std::vector<float> diff = yin_difference(frame.data(), 2048, 512);
  const std::vector<float> cmndf = yin_cmndf(diff);
  const auto non_finite = [](const std::vector<float>& v) {
    return std::count_if(v.begin(), v.end(), [](float x) { return !std::isfinite(x); });
  };
  CAPTURE(non_finite(diff));
  CAPTURE(non_finite(cmndf));

  // The difference function carries the arrival through; the normalization is
  // where it stops, on the sentinel its threshold test reaches when the running
  // sum is not finite. Asserting both ends pins WHERE the boundary is, so moving
  // it shows up here rather than downstream as a fabricated pitch.
  REQUIRE(non_finite(diff) > 0);
  REQUIRE(non_finite(cmndf) == 0);

  bool below = false;
  const float period = yin_find_pitch(cmndf, 0.2f, 22, 221, &below);
  CAPTURE(period);
  // No lag is under threshold, so the search falls back to the least dissimilar
  // one and reports it as unvoiced rather than as a detection.
  REQUIRE(!below);

  float confidence = -1.0f;
  const float freq =
      yin_with_confidence(frame.data(), 2048, 22050, 100.0f, 1000.0f, 0.2f, &confidence);
  CAPTURE(freq);
  REQUIRE(freq == 0.0f);
  REQUIRE(confidence == 0.0f);
}

TEST_CASE("yin_find_pitch honors the configured voicing threshold", "[pitch]") {
  const std::vector<float> cmndf = {1.0f, 0.9f, 0.7f, 0.4f, 0.45f, 0.8f};

  bool voiced = true;
  REQUIRE_THAT(yin_find_pitch(cmndf, 0.3f, 1, 6, &voiced), WithinAbs(3.36f, 0.01f));
  REQUIRE_FALSE(voiced);
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
  // Regression test: ensure the flat-vector Viterbi implementation still
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

  float median_f0 = result.median_f0();
  // The pitch grid is 10 cents wide and 440 Hz falls exactly between two of its bins, so the
  // median lands 5.0 cents high at 441.27 Hz -- half a bin, the largest error the grid can
  // produce. The tolerance admits that pair and excludes the next bin out.
  REQUIRE_THAT(median_f0, WithinRel(440.0f, 0.004f));

  // Count voiced frames and how many of them land within 2% of 440 Hz.
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
  // A stationary tone resolves to one grid bin for every frame: all 216 frames come back
  // voiced and all 216 carry the same f0, so neither count has any slack to give.
  REQUIRE(voiced_total == result.n_frames());
  REQUIRE(voiced_in_band == voiced_total);
}

TEST_CASE("pyin - stepped pitch tracking (Viterbi follows discontinuities)", "[pitch]") {
  // Regression test: a synthetic signal with two abrupt pitch steps
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

TEST_CASE("pitch_tuning uses ceil() for the histogram bin count like librosa", "[pitch]") {
  // librosa derives the bin count as np.ceil(1.0 / resolution), not round().
  // resolution=0.03 -> 1/0.03 = 33.33..., so librosa uses 34 bins while a
  // round()-based implementation would use 33. The frequency below has a
  // residual chosen so the two bin counts land on different histogram bins,
  // producing left-edge answers ~0.027 apart (34-bin: -0.3824, 33-bin:
  // -0.4091), well outside this test's tolerance for the wrong bin count.
  const float freq = sonare::constants::kA4Hz * std::pow(2.0f, -0.3822f / 12.0f);
  const float tuning = pitch_tuning({freq}, 0.03f, 12);
  REQUIRE_THAT(tuning, WithinAbs(-0.3824f, 0.01f));
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

TEST_CASE("yin_track matches a per-frame oracle with fresh scratch", "[pitch][yin]") {
  // yin_track carries one difference and one cmndf buffer across frames, while
  // yin_with_confidence allocates both per call. With center off, frame i is exactly
  // data + i * hop_length, so the two have to agree bit for bit: a buffer that leaked any
  // part of the previous frame would move the answer on the frames either side of a
  // transition, which is where this signal puts them.
  const int sr = 22050;
  const size_t n_samples = 11025;
  std::vector<float> samples(n_samples, 0.0f);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    if (i < n_samples / 3) {
      samples[i] = 0.5f * std::sin(sonare::constants::kTwoPi * 220.0f * t);
    } else if (i >= 2 * n_samples / 3) {
      samples[i] = 0.5f * std::sin(sonare::constants::kTwoPi * 660.0f * t);
    }
    // The middle third stays silent, so consecutive frames have nothing in common.
  }
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  PitchConfig config;
  config.center = false;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 80.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.2f;

  const PitchResult result = yin_track(audio, config);
  REQUIRE(result.n_frames() > 8);

  const float* data = audio.data();
  for (int i = 0; i < result.n_frames(); ++i) {
    float confidence = 0.0f;
    const float freq = yin_with_confidence(data + i * config.hop_length, config.frame_length, sr,
                                           config.fmin, config.fmax, config.threshold, &confidence);
    CAPTURE(i);
    REQUIRE(result.f0[i] == freq);
    REQUIRE(result.voiced_prob[i] == confidence);
  }
}

TEST_CASE("piptrack can report a peak in the topmost FFT bin", "[pitch][piptrack][edge]") {
  // librosa.util.localmax pads the spectrum with its edge value, so the topmost
  // bin is a local maximum whenever it exceeds its predecessor. Restricting the
  // scan to [1, n_bins - 2] made a peak at the Nyquist bin unreportable, which
  // only shows up once fmax reaches the Nyquist frequency -- the shipped
  // default fmax of 4000 Hz hides it at every common sample rate.
  const int sr = 8000;
  const int n_fft = 64;
  const int hop_length = 32;
  const auto n_samples = static_cast<size_t>(sr / 4);  // 0.25 s
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    // Alternating sign is a sinusoid at exactly the Nyquist frequency, so the
    // energy lands in the last bin.
    samples[i] = (i % 2 == 0) ? 0.5f : -0.5f;
  }
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  const float nyquist = 0.5f * static_cast<float>(sr);
  const PiptrackResult result = piptrack(audio, n_fft, hop_length, 1.0f, nyquist, 0.1f);
  REQUIRE(result.n_bins == n_fft / 2 + 1);
  REQUIRE(result.n_frames > 2);

  const int top = result.n_bins - 1;
  int reported_frames = 0;
  for (int t = 0; t < result.n_frames; ++t) {
    const float pitch = result.pitches[top * result.n_frames + t];
    if (pitch <= 0.0f) continue;
    ++reported_frames;
    // The shift array is zero-padded at the edges, so the peak sits exactly on
    // the bin centre rather than being extrapolated from a fabricated neighbour.
    CAPTURE(t, pitch);
    REQUIRE_THAT(pitch, WithinAbs(nyquist, 1e-3f));
    REQUIRE(result.magnitudes[top * result.n_frames + t] > 0.0f);
  }
  CAPTURE(reported_frames, result.n_frames);
  REQUIRE(reported_frames > 0);
}

TEST_CASE("piptrack never reports a peak in bin 0", "[pitch][piptrack][edge]") {
  // Bin 0 is unreportable twice over: the edge padding makes it compare
  // against itself on the "strictly greater than the left neighbour" side, and
  // fmin must be positive so 0 Hz never clears the frequency filter. Pinned so
  // that widening the scan to the bottom edge fails here rather than silently
  // diverging from librosa. A DC signal puts all the energy in bin 0.
  const int sr = 8000;
  const int n_fft = 64;
  std::vector<float> samples(static_cast<size_t>(sr / 4), 0.5f);
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  const PiptrackResult result =
      piptrack(audio, n_fft, 32, 1.0f, 0.5f * static_cast<float>(sr), 0.1f);
  REQUIRE(result.n_frames > 2);
  for (int t = 0; t < result.n_frames; ++t) {
    CAPTURE(t);
    REQUIRE(result.pitches[0 * result.n_frames + t] == 0.0f);
  }
}

TEST_CASE("piptrack matches a frame-major oracle bin for bin", "[pitch][piptrack]") {
  // The magnitude grid is [n_bins x n_frames], so the peak scan can walk it by column (a
  // frame at a time) or by row (a bin at a time). Both orders must produce the same grid
  // exactly, including where a comparison is settled by equality rather than by a margin.
  const int sr = 8000;
  const int n_fft = 64;
  const int hop_length = 16;
  const Audio audio = piptrack_traversal_fixture(sr, 4000);
  const float nyquist = 0.5f * static_cast<float>(sr);

  // What the sweep below relies on: the fixture really does produce frames whose bins are
  // exactly equal, so the equality branches are reached rather than assumed.
  {
    StftConfig cfg;
    cfg.n_fft = n_fft;
    cfg.hop_length = hop_length;
    cfg.win_length = n_fft;
    cfg.center = true;
    const Spectrogram spec = Spectrogram::compute(audio, cfg);
    const std::vector<float>& mag = spec.magnitude();
    int silent_frames = 0;
    int tied_adjacent_bins = 0;
    for (int t = 0; t < spec.n_frames(); ++t) {
      bool silent = true;
      for (int k = 0; k < spec.n_bins(); ++k) {
        const float value = mag[k * spec.n_frames() + t];
        if (value != 0.0f) silent = false;
        if (k > 0 && value == mag[(k - 1) * spec.n_frames() + t]) ++tied_adjacent_bins;
      }
      if (silent) ++silent_frames;
    }
    CAPTURE(silent_frames, tied_adjacent_bins);
    REQUIRE(silent_frames > 0);
    REQUIRE(tied_adjacent_bins > 0);
  }

  struct Params {
    float fmin;
    float fmax;
    float threshold;
  };
  // threshold == 1 puts the gate exactly on the frame maximum, so the winning bin clears it
  // by equality; threshold == 0 admits every local maximum in the band.
  const std::vector<Params> sweep = {
      {1.0f, nyquist, 0.1f}, {1.0f, nyquist, 1.0f}, {1.0f, nyquist, 0.0f}, {250.0f, 2000.0f, 0.5f}};

  for (const Params& params : sweep) {
    CAPTURE(params.fmin, params.fmax, params.threshold);
    const PiptrackResult got =
        piptrack(audio, n_fft, hop_length, params.fmin, params.fmax, params.threshold);
    const PiptrackResult want =
        oracle_piptrack(audio, n_fft, hop_length, params.fmin, params.fmax, params.threshold);

    REQUIRE(got.n_bins == want.n_bins);
    REQUIRE(got.n_frames == want.n_frames);
    REQUIRE(got.pitches.size() == want.pitches.size());
    REQUIRE(got.magnitudes.size() == want.magnitudes.size());

    size_t mismatch = want.pitches.size();
    int reported_peaks = 0;
    for (size_t i = 0; i < want.pitches.size(); ++i) {
      if (want.pitches[i] != 0.0f) ++reported_peaks;
      if (mismatch == want.pitches.size() &&
          (got.pitches[i] != want.pitches[i] || got.magnitudes[i] != want.magnitudes[i])) {
        mismatch = i;
      }
    }
    if (mismatch != want.pitches.size()) {
      const int bin = static_cast<int>(mismatch) / want.n_frames;
      const int frame = static_cast<int>(mismatch) % want.n_frames;
      CAPTURE(bin, frame);
      REQUIRE(got.pitches[mismatch] == want.pitches[mismatch]);
      REQUIRE(got.magnitudes[mismatch] == want.magnitudes[mismatch]);
    }
    // A parameter set that reports nothing would compare two empty grids.
    CAPTURE(reported_peaks);
    REQUIRE(reported_peaks > 0);
  }
}

TEST_CASE("pitch_tuning spans [-0.5, 0.5) with -0.5 attainable", "[pitch][tuning]") {
  // The documented interval. A residual is folded by `if (frac >= 0.5) frac -= 1`,
  // so it lands in [-0.5, 0.5): exactly -0.5 is a legitimate answer and +0.5 can
  // never be produced. A caller that took the old docs literally and asserted
  // `-0.5 < tuning <= 0.5` would reject a correct result.
  //
  // A pitch half a bin flat is the case that produces it: 12 * log2(f / A4) is
  // -0.5, whose fractional part folds to -0.5 and selects histogram bin 0, whose
  // left edge is -0.5. librosa.pitch_tuning answers -0.5 here too.
  const float half_bin_flat = sonare::constants::kA4Hz * std::pow(2.0f, -0.5f / 12.0f);
  REQUIRE_THAT(pitch_tuning({half_bin_flat}, 0.01f, 12), WithinAbs(-0.5f, 1e-6f));

  // A residual of exactly +0.5 folds to -0.5, so +0.5 is unattainable. The fold
  // turns on a >= comparison, so a frequency a single float step below the half
  // bin lands on the other side of it and answers +0.49 instead -- which is
  // what librosa answers for that same value, so the knife edge is the shared
  // definition rather than a divergence. The sweep below pins the interval; the
  // exact boundary value is deliberately not asserted.

  // An in-tune reference sits at 0, and nothing ever reaches +0.5.
  REQUIRE_THAT(pitch_tuning({sonare::constants::kA4Hz}, 0.01f, 12), WithinAbs(0.0f, 1e-6f));
  for (int cents = -49; cents <= 49; ++cents) {
    const float freq =
        sonare::constants::kA4Hz * std::pow(2.0f, static_cast<float>(cents) / 1200.0f);
    const float tuning = pitch_tuning({freq}, 0.01f, 12);
    CAPTURE(cents);
    REQUIRE(tuning >= -0.5f);
    REQUIRE(tuning < 0.5f);
  }
}

TEST_CASE("yin_track and pyin reject the same PitchConfig domains", "[pitch][validation]") {
  // Both engines are reachable from one public entry point (the pitch command's
  // --algorithm switch), so a config one rejects must be rejected by the other.
  // yin_track used to accept fmin >= fmax and answer "0 Hz, unvoiced" for every
  // frame, which reads as a valid analysis rather than as swapped arguments.
  const std::vector<float> tone = generate_sine(22050, 440.0f, 22050);
  Audio audio = Audio::from_buffer(tone.data(), tone.size(), 22050);

  const std::vector<std::pair<float, float>> invalid = {
      {3000.0f, 500.0f},  // swapped
      {500.0f, 500.0f},   // empty band
      {-50.0f, 2093.0f},  // negative fmin: sr / fmin is meaningless
      {0.0f, 2093.0f},    // zero fmin
  };
  for (const auto& [fmin, fmax] : invalid) {
    CAPTURE(fmin);
    CAPTURE(fmax);
    PitchConfig config;
    config.fmin = fmin;
    config.fmax = fmax;
    REQUIRE_THROWS_AS(yin_track(audio, config), SonareException);
    REQUIRE_THROWS_AS(pyin(audio, config), SonareException);
  }

  // The default band is still accepted by both.
  PitchConfig ok;
  REQUIRE_NOTHROW(yin_track(audio, ok));
  REQUIRE_NOTHROW(pyin(audio, ok));

  // The verdict is a property of the arguments, not of the signal length: a
  // buffer too short to yield a single frame must still reject a bad band.
  const std::vector<float> short_tone = generate_sine(220, 440.0f, 22050);
  Audio tiny = Audio::from_buffer(short_tone.data(), short_tone.size(), 22050);
  PitchConfig swapped;
  swapped.fmin = 3000.0f;
  swapped.fmax = 500.0f;
  REQUIRE_THROWS_AS(yin_track(tiny, swapped), SonareException);
  REQUIRE_THROWS_AS(pyin(tiny, swapped), SonareException);
}

TEST_CASE("pyin resolves a single-frame input", "[pitch][edge]") {
  // The Viterbi forward pass never runs when there is only one frame, so the best-path
  // argmax has to read the initial observation row rather than whatever a per-frame
  // buffer happens to hold. Exactly one frame: center off, and the signal is exactly
  // one frame long.
  PitchConfig config;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  config.threshold = 0.3f;
  config.frame_length = 2048;
  config.hop_length = 512;
  config.center = false;

  const int sr = 22050;
  Audio audio =
      Audio::from_vector(sonare::test::generate_sine_samples(440.0f, sr, config.frame_length), sr);
  REQUIRE(audio.size() == static_cast<size_t>(config.frame_length));

  PitchResult result = pyin(audio, config);

  REQUIRE(result.n_frames() == 1);
  REQUIRE(result.voiced_flag[0]);
  // A row that was never written reads as all -inf, whose argmax is state 0 -- i.e. fmin.
  // Landing on 440 Hz is what separates the two.
  REQUIRE_THAT(result.f0[0], WithinRel(440.0f, 0.03f));
}

// The local-max test compares b against its neighbours and the gate, and a
// non-finite answers false to all of them, so the bin is admitted as a peak.
// Below the top edge that self-corrects -- the parabolic shift goes non-finite
// too and the frequency with it, so the pitch is not positive and nothing reads
// the magnitude. At the top edge the shift is a fixed 0, so the frequency stays
// finite and a positive pitch is reported carrying a NaN magnitude. Reaching it
// needs fmax at Nyquist, which the default band excludes.
TEST_CASE("piptrack does not report a positive pitch with a non-finite magnitude",
          "[pitch][edge]") {
  const int sr = 22050;
  const int n = 8192;
  auto alternating = [&](float amp) {
    std::vector<float> samples(static_cast<size_t>(n));
    // Alternating sign puts the energy in the Nyquist bin, the one treated as the
    // top edge; at this amplitude the spectrum itself goes non-finite.
    for (int i = 0; i < n; ++i) samples[static_cast<size_t>(i)] = (i % 2 == 0) ? amp : -amp;
    return Audio::from_vector(std::move(samples), sr);
  };
  auto count = [](const PiptrackResult& pp) {
    std::pair<size_t, size_t> counts{0, 0};
    for (size_t i = 0; i < pp.pitches.size(); ++i) {
      if (pp.pitches[i] > 0.0f) {
        ++counts.first;
        if (!std::isfinite(pp.magnitudes[i])) ++counts.second;
      }
    }
    return counts;
  };

  SECTION("a spectrum that has gone non-finite yields no poisoned peak") {
    const auto counts = count(piptrack(alternating(1e38f), 2048, 512, 150.0f, 11025.0f, 0.1f));
    REQUIRE(counts.second == 0);
  }

  SECTION("the non-vacuity control: the same band on a finite spectrum still reports peaks") {
    // Without this the case above is satisfied by returning nothing at all, which
    // is what an over-broad refusal would do.
    const auto counts = count(piptrack(alternating(1e20f), 2048, 512, 150.0f, 11025.0f, 0.1f));
    REQUIRE(counts.first > 0);
    REQUIRE(counts.second == 0);
  }
}
