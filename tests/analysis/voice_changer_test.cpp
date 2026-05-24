#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "analysis/voice_changer/voice_changer_core.h"
#include "core/audio.h"
#include "core/fft.h"
#include "util/constants.h"

using Catch::Matchers::WithinRel;
using namespace sonare::analysis::voice_changer;

namespace {

std::vector<float> sine(float frequency_hz, int sample_rate, int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] =
        0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                           static_cast<double>(i) / sample_rate));
  }
  return output;
}

int zero_crossings(const std::vector<float>& samples) {
  int crossings = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if ((samples[i - 1] <= 0.0f && samples[i] > 0.0f) ||
        (samples[i - 1] >= 0.0f && samples[i] < 0.0f)) {
      ++crossings;
    }
  }
  return crossings;
}

// Magnitude-weighted spectral centroid (Hz) of a windowed signal segment.
float spectral_centroid(const std::vector<float>& samples, int sample_rate) {
  constexpr int kNfft = 4096;
  std::vector<float> frame(static_cast<size_t>(kNfft), 0.0f);
  const int n = std::min(kNfft, static_cast<int>(samples.size()));
  for (int i = 0; i < n; ++i) {
    // Hann window to limit spectral leakage.
    const float w = 0.5f - 0.5f * std::cos(sonare::constants::kTwoPi * static_cast<float>(i) /
                                            static_cast<float>(kNfft - 1));
    frame[static_cast<size_t>(i)] = samples[static_cast<size_t>(i)] * w;
  }
  sonare::FFT fft(kNfft);
  std::vector<std::complex<float>> spec(static_cast<size_t>(fft.n_bins()));
  fft.forward(frame.data(), spec.data());

  double weighted = 0.0;
  double total = 0.0;
  const double bin_hz = static_cast<double>(sample_rate) / kNfft;
  for (int b = 0; b < fft.n_bins(); ++b) {
    const double mag = std::abs(spec[static_cast<size_t>(b)]);
    weighted += mag * (b * bin_hz);
    total += mag;
  }
  return total > 0.0 ? static_cast<float>(weighted / total) : 0.0f;
}

// Dominant fundamental frequency (Hz) via autocorrelation peak search.
float dominant_frequency(const std::vector<float>& samples, int sample_rate, float fmin,
                         float fmax) {
  const int min_lag = static_cast<int>(static_cast<float>(sample_rate) / fmax);
  const int max_lag = static_cast<int>(static_cast<float>(sample_rate) / fmin);
  const int n = static_cast<int>(samples.size());
  double best = -1.0;
  int best_lag = min_lag;
  for (int lag = min_lag; lag <= max_lag && lag < n; ++lag) {
    double acc = 0.0;
    for (int i = 0; i + lag < n; ++i) {
      acc += static_cast<double>(samples[static_cast<size_t>(i)]) *
             static_cast<double>(samples[static_cast<size_t>(i + lag)]);
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return static_cast<float>(sample_rate) / static_cast<float>(best_lag);
}

}  // namespace

TEST_CASE("StreamingRetune shifts block pitch up an octave", "[voice_changer]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = 32768;  // Long enough to flush the grain latency.
  constexpr float f0 = 220.0f;
  constexpr int block = 512;
  const auto input = sine(f0, sample_rate, samples);
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);

  StreamingRetune retune({12.0f, 1.0f});  // +1 octave, fully wet.
  retune.prepare(sample_rate, block);

  // Stream block-by-block, respecting max_block_size from prepare().
  for (int pos = 0; pos < samples; pos += block) {
    const int n = std::min(block, samples - pos);
    retune.process_block(input.data() + pos, output.data() + pos, n);
  }

  for (float sample : output) {
    REQUIRE(std::isfinite(sample));
  }
  REQUIRE(zero_crossings(output) > zero_crossings(input));

  // Estimate the dominant output frequency past the initial latency region
  // (~grain_size). It should be about 2 * f0 (one octave up).
  const std::vector<float> steady(output.begin() + 8192, output.end());
  const float dominant = dominant_frequency(steady, sample_rate, 200.0f, 800.0f);
  REQUIRE_THAT(dominant, WithinRel(2.0f * f0, 0.08f));
}

TEST_CASE("FormantWarp raises the spectral envelope when factor > 1", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  constexpr int n = sample_rate / 2;
  constexpr float f0 = 150.0f;
  // Vowel-like source: harmonics of f0 with a formant-shaped magnitude envelope
  // peaking near 900 Hz. This gives a clear spectral envelope to warp.
  std::vector<float> samples(static_cast<size_t>(n), 0.0f);
  constexpr float formant_hz = 900.0f;
  constexpr float bandwidth_hz = 600.0f;
  for (int h = 1; h * f0 < static_cast<float>(n); ++h) {
    const float harm_hz = h * f0;
    const float env = 1.0f / (1.0f + std::pow((harm_hz - formant_hz) / bandwidth_hz, 2.0f));
    for (int i = 0; i < n; ++i) {
      samples[static_cast<size_t>(i)] +=
          0.2f * env *
          static_cast<float>(std::sin(sonare::constants::kTwoPiD * harm_hz *
                                      static_cast<double>(i) / sample_rate));
    }
  }
  const sonare::Audio audio = sonare::Audio::from_vector(std::vector<float>(samples), sample_rate);

  FormantWarp warp({1.3f, 12, 1.0f});  // Raise formants.
  const sonare::Audio warped = warp.process(audio);

  REQUIRE(warped.size() == audio.size());
  REQUIRE(warped.sample_rate() == audio.sample_rate());
  for (float sample : warped) {
    REQUIRE(std::isfinite(sample));
  }

  // Measure spectral centroid over a steady mid-signal segment.
  const int start = n / 4;
  const std::vector<float> in_seg(samples.begin() + start, samples.end());
  std::vector<float> out_vec(warped.data(), warped.data() + warped.size());
  const std::vector<float> out_seg(out_vec.begin() + start, out_vec.end());

  const float centroid_in = spectral_centroid(in_seg, sample_rate);
  const float centroid_out = spectral_centroid(out_seg, sample_rate);
  REQUIRE(centroid_in > 0.0f);
  // Raising formants pushes spectral energy upward.
  REQUIRE(centroid_out > centroid_in);
}

TEST_CASE("VoiceChanger combines pitch and formant controls", "[voice_changer]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(220.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  VoiceChangerConfig config;
  config.pitch_semitones = 7.0f;
  config.formant_factor = 1.15f;
  VoiceChanger changer(config);
  const sonare::Audio changed = changer.process(audio);

  REQUIRE(!changed.empty());
  REQUIRE(changed.sample_rate() == audio.sample_rate());
  REQUIRE_THAT(changed.duration(), WithinRel(audio.duration(), 0.05f));
}
