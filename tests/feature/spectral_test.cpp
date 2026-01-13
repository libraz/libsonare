/// @file spectral_test.cpp
/// @brief Tests for spectral feature functions.

#include "feature/spectral.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a simple test signal (sine wave).
Audio create_sine_audio(float freq, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates white noise signal.
Audio create_noise_audio(int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  // Simple LCG for reproducible pseudo-random numbers
  uint32_t seed = 12345;
  for (int i = 0; i < n_samples; ++i) {
    seed = seed * 1103515245 + 12345;
    samples[i] = static_cast<float>(seed % 1000) / 500.0f - 1.0f;
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("spectral_centroid basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);
  int sr = audio.sample_rate();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> centroid = spectral_centroid(spec, sr);

  REQUIRE(centroid.size() == static_cast<size_t>(spec.n_frames()));

  // Centroid should be near 1000 Hz for 1000 Hz sine
  float mean_centroid = 0.0f;
  for (float c : centroid) {
    mean_centroid += c;
  }
  mean_centroid /= static_cast<float>(centroid.size());

  REQUIRE_THAT(mean_centroid, WithinRel(1000.0f, 0.2f));
}

TEST_CASE("spectral_centroid low vs high frequency", "[spectral]") {
  Audio low_audio = create_sine_audio(200.0f);
  Audio high_audio = create_sine_audio(4000.0f);
  int sr = 22050;

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram low_spec = Spectrogram::compute(low_audio, config);
  Spectrogram high_spec = Spectrogram::compute(high_audio, config);

  std::vector<float> low_centroid = spectral_centroid(low_spec, sr);
  std::vector<float> high_centroid = spectral_centroid(high_spec, sr);

  // Average centroids
  float low_mean = 0.0f, high_mean = 0.0f;
  for (float c : low_centroid) low_mean += c;
  for (float c : high_centroid) high_mean += c;
  low_mean /= static_cast<float>(low_centroid.size());
  high_mean /= static_cast<float>(high_centroid.size());

  // High frequency signal should have higher centroid
  REQUIRE(high_mean > low_mean * 2.0f);
}

TEST_CASE("spectral_bandwidth basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);
  int sr = audio.sample_rate();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> bandwidth = spectral_bandwidth(spec, sr);

  REQUIRE(bandwidth.size() == static_cast<size_t>(spec.n_frames()));

  // Bandwidth should be positive
  for (float b : bandwidth) {
    REQUIRE(b >= 0.0f);
  }

  // Pure sine should have narrow bandwidth
  float mean_bw = 0.0f;
  for (float b : bandwidth) {
    mean_bw += b;
  }
  mean_bw /= static_cast<float>(bandwidth.size());

  // Should be relatively narrow (< 500 Hz for pure sine)
  REQUIRE(mean_bw < 500.0f);
}

TEST_CASE("spectral_rolloff basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);
  int sr = audio.sample_rate();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> rolloff = spectral_rolloff(spec, sr, 0.85f);

  REQUIRE(rolloff.size() == static_cast<size_t>(spec.n_frames()));

  // Rolloff should be near 1000 Hz for pure sine
  float mean_rolloff = 0.0f;
  for (float r : rolloff) {
    mean_rolloff += r;
  }
  mean_rolloff /= static_cast<float>(rolloff.size());

  REQUIRE_THAT(mean_rolloff, WithinRel(1000.0f, 0.3f));
}

TEST_CASE("spectral_rolloff percentage parameter", "[spectral]") {
  Audio audio = create_noise_audio();
  int sr = audio.sample_rate();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> rolloff_85 = spectral_rolloff(spec, sr, 0.85f);
  std::vector<float> rolloff_50 = spectral_rolloff(spec, sr, 0.50f);

  // 50% rolloff should be lower than 85% rolloff
  float mean_85 = 0.0f, mean_50 = 0.0f;
  for (size_t i = 0; i < rolloff_85.size(); ++i) {
    mean_85 += rolloff_85[i];
    mean_50 += rolloff_50[i];
  }
  mean_85 /= static_cast<float>(rolloff_85.size());
  mean_50 /= static_cast<float>(rolloff_50.size());

  REQUIRE(mean_50 < mean_85);
}

TEST_CASE("spectral_flatness sine vs noise", "[spectral]") {
  Audio sine_audio = create_sine_audio(1000.0f);
  Audio noise_audio = create_noise_audio();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram sine_spec = Spectrogram::compute(sine_audio, config);
  Spectrogram noise_spec = Spectrogram::compute(noise_audio, config);

  std::vector<float> sine_flatness = spectral_flatness(sine_spec);
  std::vector<float> noise_flatness = spectral_flatness(noise_spec);

  float sine_mean = 0.0f, noise_mean = 0.0f;
  for (float f : sine_flatness) sine_mean += f;
  for (float f : noise_flatness) noise_mean += f;
  sine_mean /= static_cast<float>(sine_flatness.size());
  noise_mean /= static_cast<float>(noise_flatness.size());

  // Noise should have higher flatness than pure sine
  REQUIRE(noise_mean > sine_mean);
  // Flatness should be in [0, 1]
  REQUIRE(sine_mean >= 0.0f);
  REQUIRE(noise_mean <= 1.0f);
}

TEST_CASE("spectral_contrast basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);
  int sr = audio.sample_rate();

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  int n_bands = 6;
  std::vector<float> contrast = spectral_contrast(spec, sr, n_bands);

  // Should have (n_bands + 1) x n_frames values
  REQUIRE(contrast.size() == static_cast<size_t>((n_bands + 1) * spec.n_frames()));

  // Values should be finite
  for (float c : contrast) {
    REQUIRE(std::isfinite(c));
  }
}

TEST_CASE("zero_crossing_rate basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);

  int frame_length = 2048;
  int hop_length = 512;

  std::vector<float> zcr = zero_crossing_rate(audio, frame_length, hop_length);

  // Should have frames
  REQUIRE(!zcr.empty());

  // ZCR should be in [0, 1]
  for (float z : zcr) {
    REQUIRE(z >= 0.0f);
    REQUIRE(z <= 1.0f);
  }
}

TEST_CASE("zero_crossing_rate frequency relationship", "[spectral]") {
  // Higher frequency should have more zero crossings
  Audio low_audio = create_sine_audio(100.0f);
  Audio high_audio = create_sine_audio(4000.0f);

  int frame_length = 2048;
  int hop_length = 512;

  std::vector<float> low_zcr = zero_crossing_rate(low_audio, frame_length, hop_length);
  std::vector<float> high_zcr = zero_crossing_rate(high_audio, frame_length, hop_length);

  float low_mean = 0.0f, high_mean = 0.0f;
  for (float z : low_zcr) low_mean += z;
  for (float z : high_zcr) high_mean += z;
  low_mean /= static_cast<float>(low_zcr.size());
  high_mean /= static_cast<float>(high_zcr.size());

  // Higher frequency should have more zero crossings
  REQUIRE(high_mean > low_mean);
}

TEST_CASE("rms_energy basic", "[spectral]") {
  Audio audio = create_sine_audio(1000.0f);

  int frame_length = 2048;
  int hop_length = 512;

  std::vector<float> rms = rms_energy(audio, frame_length, hop_length);

  // Should have frames
  REQUIRE(!rms.empty());

  // RMS should be non-negative
  for (float r : rms) {
    REQUIRE(r >= 0.0f);
  }

  // For a sine wave with amplitude 1, RMS should be around 1/sqrt(2) ≈ 0.707
  float mean_rms = 0.0f;
  for (float r : rms) {
    mean_rms += r;
  }
  mean_rms /= static_cast<float>(rms.size());

  REQUIRE_THAT(mean_rms, WithinRel(0.707f, 0.1f));
}

TEST_CASE("rms_energy silence vs loud", "[spectral]") {
  // Create silent and loud audio
  int sr = 22050;
  int n_samples = 11025;  // 0.5 sec

  std::vector<float> silent(n_samples, 0.0f);
  std::vector<float> loud(n_samples);
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    loud[i] = std::sin(2.0f * M_PI * 440.0f * t);
  }

  Audio silent_audio = Audio::from_vector(std::move(silent), sr);
  Audio loud_audio = Audio::from_vector(std::move(loud), sr);

  std::vector<float> silent_rms = rms_energy(silent_audio);
  std::vector<float> loud_rms = rms_energy(loud_audio);

  float silent_mean = 0.0f, loud_mean = 0.0f;
  for (float r : silent_rms) silent_mean += r;
  for (float r : loud_rms) loud_mean += r;
  if (!silent_rms.empty()) silent_mean /= static_cast<float>(silent_rms.size());
  if (!loud_rms.empty()) loud_mean /= static_cast<float>(loud_rms.size());

  // Silent should be zero, loud should be positive
  REQUIRE(silent_mean < 0.001f);
  REQUIRE(loud_mean > 0.5f);
}
