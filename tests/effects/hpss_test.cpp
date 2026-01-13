/// @file hpss_test.cpp
/// @brief Tests for HPSS (Harmonic-Percussive Source Separation).

#include "effects/hpss.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a harmonic signal (single sine wave).
Audio create_harmonic_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a percussive signal (clicks/impulses).
Audio create_percussive_audio(int sr = 22050, float duration = 0.5f, int n_clicks = 5) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  int click_length = sr / 500;  // 2ms click
  int interval = n_samples / n_clicks;

  for (int c = 0; c < n_clicks; ++c) {
    int start = c * interval;
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("median_filter_horizontal basic", "[hpss]") {
  int n_bins = 5;
  int n_frames = 10;
  std::vector<float> input(n_bins * n_frames);

  // Create a simple pattern
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      input[k * n_frames + t] = static_cast<float>(t);
    }
  }

  std::vector<float> output = median_filter_horizontal(input.data(), n_bins, n_frames, 3);

  REQUIRE(output.size() == input.size());

  // Check that median filter smooths values
  for (float val : output) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("median_filter_vertical basic", "[hpss]") {
  int n_bins = 10;
  int n_frames = 5;
  std::vector<float> input(n_bins * n_frames);

  // Create a simple pattern
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      input[k * n_frames + t] = static_cast<float>(k);
    }
  }

  std::vector<float> output = median_filter_vertical(input.data(), n_bins, n_frames, 3);

  REQUIRE(output.size() == input.size());

  // Check that median filter produces finite values
  for (float val : output) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("hpss spectrogram basic", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResult result = hpss(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(result.harmonic.n_frames() == spec.n_frames());
  REQUIRE(result.percussive.n_frames() == spec.n_frames());
}

TEST_CASE("hpss audio basic", "[hpss]") {
  Audio audio = create_harmonic_audio();

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssAudioResult result = hpss(audio, config, stft_config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());

  // Output should have similar length to input
  REQUIRE_THAT(static_cast<float>(result.harmonic.size()),
               WithinRel(static_cast<float>(audio.size()), 0.1f));
}

TEST_CASE("hpss soft vs hard mask", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig soft_config;
  soft_config.use_soft_mask = true;

  HpssConfig hard_config;
  hard_config.use_soft_mask = false;

  HpssSpectrogramResult soft = hpss(spec, soft_config);
  HpssSpectrogramResult hard = hpss(spec, hard_config);

  // Both should produce valid results
  REQUIRE(!soft.harmonic.empty());
  REQUIRE(!hard.harmonic.empty());

  // Soft and hard masks should produce different results
  const float* soft_mag = soft.harmonic.magnitude().data();
  const float* hard_mag = hard.harmonic.magnitude().data();

  bool different = false;
  for (int i = 0; i < soft.harmonic.n_bins() * soft.harmonic.n_frames(); ++i) {
    if (std::abs(soft_mag[i] - hard_mag[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);
}

TEST_CASE("harmonic helper function", "[hpss]") {
  Audio audio = create_harmonic_audio();

  Audio harm = harmonic(audio);

  REQUIRE(!harm.empty());
  REQUIRE(harm.sample_rate() == audio.sample_rate());
}

TEST_CASE("percussive helper function", "[hpss]") {
  Audio audio = create_percussive_audio();

  Audio perc = percussive(audio);

  REQUIRE(!perc.empty());
  REQUIRE(perc.sample_rate() == audio.sample_rate());
}

TEST_CASE("hpss_with_residual spectrogram", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResultWithResidual result = hpss_with_residual(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());
  REQUIRE(result.harmonic.n_frames() == spec.n_frames());
  REQUIRE(result.percussive.n_frames() == spec.n_frames());
  REQUIRE(result.residual.n_frames() == spec.n_frames());
}

TEST_CASE("hpss_with_residual audio", "[hpss]") {
  Audio audio = create_harmonic_audio();

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());

  REQUIRE(result.harmonic.sample_rate() == audio.sample_rate());
  REQUIRE(result.percussive.sample_rate() == audio.sample_rate());
  REQUIRE(result.residual.sample_rate() == audio.sample_rate());
}

TEST_CASE("residual helper function", "[hpss]") {
  Audio audio = create_harmonic_audio();

  Audio res = residual(audio);

  REQUIRE(!res.empty());
  REQUIRE(res.sample_rate() == audio.sample_rate());
}

TEST_CASE("hpss_with_residual hard mask", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  HpssConfig config;
  config.use_soft_mask = false;

  HpssSpectrogramResultWithResidual result = hpss_with_residual(spec, config);

  REQUIRE(!result.harmonic.empty());
  REQUIRE(!result.percussive.empty());
  REQUIRE(!result.residual.empty());
}
