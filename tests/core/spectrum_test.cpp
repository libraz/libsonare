/// @file spectrum_test.cpp
/// @brief Tests for STFT/iSTFT and Spectrogram class.

#include "core/spectrum.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
}

float compute_snr(const float* original, const float* reconstructed, size_t size) {
  float signal_power = 0.0f;
  float noise_power = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    signal_power += original[i] * original[i];
    float diff = original[i] - reconstructed[i];
    noise_power += diff * diff;
  }
  if (noise_power < 1e-10f) {
    return 100.0f;  // Very high SNR
  }
  return 10.0f * std::log10(signal_power / noise_power);
}
}  // namespace

TEST_CASE("Spectrogram compute basic", "[spectrum]") {
  constexpr int sr = 22050;
  constexpr int samples = sr;  // 1 second
  std::vector<float> sine = generate_sine(samples, 440.0f, sr);
  Audio audio = Audio::from_vector(std::move(sine), sr);

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);

  REQUIRE_FALSE(spec.empty());
  REQUIRE(spec.n_bins() == config.n_fft / 2 + 1);
  REQUIRE(spec.n_fft() == config.n_fft);
  REQUIRE(spec.hop_length() == config.hop_length);
  REQUIRE(spec.sample_rate() == sr);
  REQUIRE(spec.n_frames() > 0);
}

TEST_CASE("Spectrogram magnitude and power", "[spectrum]") {
  constexpr int sr = 22050;
  std::vector<float> sine = generate_sine(sr / 2, 440.0f, sr);
  Audio audio = Audio::from_vector(std::move(sine), sr);

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, config);

  const std::vector<float>& mag = spec.magnitude();
  const std::vector<float>& pwr = spec.power();

  REQUIRE(mag.size() == static_cast<size_t>(spec.n_bins() * spec.n_frames()));
  REQUIRE(pwr.size() == mag.size());

  // Power should be magnitude squared (use relative tolerance for larger values)
  for (size_t i = 0; i < mag.size(); ++i) {
    float expected = mag[i] * mag[i];
    if (expected > 1.0f) {
      REQUIRE_THAT(pwr[i], WithinRel(expected, 1e-5f));
    } else {
      REQUIRE_THAT(pwr[i], WithinAbs(expected, 1e-6f));
    }
  }
}

TEST_CASE("Spectrogram to_db", "[spectrum]") {
  constexpr int sr = 22050;
  std::vector<float> sine = generate_sine(sr / 4, 440.0f, sr);
  Audio audio = Audio::from_vector(std::move(sine), sr);

  StftConfig config;
  config.n_fft = 512;
  config.hop_length = 128;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> db = spec.to_db();

  REQUIRE(db.size() == static_cast<size_t>(spec.n_bins() * spec.n_frames()));

  // dB values should be finite
  for (float val : db) {
    REQUIRE(std::isfinite(val));
  }
}

TEST_CASE("STFT/iSTFT roundtrip", "[spectrum]") {
  constexpr int sr = 22050;
  constexpr int samples = sr;  // 1 second
  std::vector<float> original = generate_sine(samples, 440.0f, sr);
  Audio audio = Audio::from_vector(std::vector<float>(original), sr);

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.center = true;

  // Forward STFT
  Spectrogram spec = Spectrogram::compute(audio, config);

  // Inverse STFT
  Audio reconstructed = spec.to_audio(samples);

  REQUIRE_FALSE(reconstructed.empty());

  // Compare middle section (skip edges due to windowing)
  size_t compare_length = std::min(original.size(), reconstructed.size());
  size_t skip = config.n_fft;  // Skip more at edges
  if (compare_length > 2 * skip) {
    float snr =
        compute_snr(original.data() + skip, reconstructed.data() + skip, compare_length - 2 * skip);
    // Require SNR > 20 dB (relaxed threshold)
    REQUIRE(snr > 20.0f);
  }
}

TEST_CASE("STFT/iSTFT with different window sizes", "[spectrum]") {
  constexpr int sr = 22050;
  std::vector<float> original = generate_sine(sr / 2, 880.0f, sr);
  Audio audio = Audio::from_vector(std::vector<float>(original), sr);

  SECTION("n_fft = 1024, hop = 256") {
    StftConfig config;
    config.n_fft = 1024;
    config.hop_length = 256;

    Spectrogram spec = Spectrogram::compute(audio, config);
    Audio reconstructed = spec.to_audio();

    REQUIRE_FALSE(reconstructed.empty());
    REQUIRE(reconstructed.sample_rate() == sr);
  }

  SECTION("n_fft = 4096, hop = 1024") {
    StftConfig config;
    config.n_fft = 4096;
    config.hop_length = 1024;

    Spectrogram spec = Spectrogram::compute(audio, config);
    Audio reconstructed = spec.to_audio();

    REQUIRE_FALSE(reconstructed.empty());
  }
}

TEST_CASE("Spectrogram empty audio", "[spectrum]") {
  Audio empty;
  Spectrogram spec = Spectrogram::compute(empty);

  REQUIRE(spec.empty());
  REQUIRE(spec.n_bins() == 0);
  REQUIRE(spec.n_frames() == 0);
}

TEST_CASE("Spectrogram DC signal", "[spectrum]") {
  constexpr int sr = 22050;
  constexpr int samples = 4096;

  // DC signal (constant value)
  std::vector<float> dc(samples, 0.5f);
  Audio audio = Audio::from_vector(std::move(dc), sr);

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, config);
  const std::vector<float>& mag = spec.magnitude();

  // DC component (bin 0) should dominate
  for (int t = 0; t < spec.n_frames(); ++t) {
    float dc_mag = mag[0 * spec.n_frames() + t];  // Bin 0

    // DC should be larger than other bins (at least for middle frames)
    float other_sum = 0.0f;
    for (int f = 1; f < spec.n_bins(); ++f) {
      other_sum += mag[f * spec.n_frames() + t];
    }

    // DC should be significant relative to other bins
    if (t > 0 && t < spec.n_frames() - 1) {
      REQUIRE(dc_mag > other_sum * 0.1f);  // Relaxed check
    }
  }
}

TEST_CASE("Spectrogram sine wave frequency detection", "[spectrum]") {
  constexpr int sr = 22050;
  constexpr float freq = 1000.0f;  // 1 kHz
  constexpr int samples = sr;

  std::vector<float> sine = generate_sine(samples, freq, sr);
  Audio audio = Audio::from_vector(std::move(sine), sr);

  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, config);
  const std::vector<float>& mag = spec.magnitude();

  // Find expected bin for 1 kHz
  float bin_hz = static_cast<float>(sr) / config.n_fft;
  int expected_bin = static_cast<int>(std::round(freq / bin_hz));

  // Check that peak is near expected bin (for middle frames)
  int mid_frame = spec.n_frames() / 2;
  float max_mag = 0.0f;
  int max_bin = 0;

  for (int f = 0; f < spec.n_bins(); ++f) {
    float m = mag[f * spec.n_frames() + mid_frame];
    if (m > max_mag) {
      max_mag = m;
      max_bin = f;
    }
  }

  // Peak should be within 2 bins of expected
  REQUIRE(std::abs(max_bin - expected_bin) <= 2);
}

TEST_CASE("Griffin-Lim basic reconstruction", "[spectrum]") {
  constexpr int sr = 22050;
  constexpr int samples = sr / 2;  // 0.5 seconds
  std::vector<float> original = generate_sine(samples, 440.0f, sr);
  Audio audio = Audio::from_vector(std::vector<float>(original), sr);

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  // Compute magnitude spectrogram
  Spectrogram spec = Spectrogram::compute(audio, config);
  const std::vector<float>& mag = spec.magnitude();

  // Reconstruct with Griffin-Lim
  GriffinLimConfig gl_config;
  gl_config.n_iter = 32;
  gl_config.momentum = 0.0f;  // Disable momentum for simpler test

  Audio reconstructed = griffin_lim(mag, spec.n_bins(), spec.n_frames(), config.n_fft,
                                    config.hop_length, sr, gl_config);

  REQUIRE_FALSE(reconstructed.empty());
  REQUIRE(reconstructed.sample_rate() == sr);

  // Griffin-Lim should produce output with similar RMS (not total energy, as lengths may differ)
  float orig_rms = 0.0f;
  for (float s : original) {
    orig_rms += s * s;
  }
  orig_rms = std::sqrt(orig_rms / samples);

  float recon_rms = 0.0f;
  for (size_t i = 0; i < reconstructed.size(); ++i) {
    recon_rms += reconstructed[i] * reconstructed[i];
  }
  recon_rms = std::sqrt(recon_rms / reconstructed.size());

  // RMS should be within reasonable range (Griffin-Lim is approximate)
  float rms_ratio = recon_rms / orig_rms;
  REQUIRE(rms_ratio > 0.3f);
  REQUIRE(rms_ratio < 3.0f);
}

TEST_CASE("Spectrogram from_complex", "[spectrum]") {
  constexpr int n_bins = 5;
  constexpr int n_frames = 3;
  constexpr int n_fft = 8;
  constexpr int hop_length = 4;
  constexpr int sr = 22050;

  std::vector<std::complex<float>> data(n_bins * n_frames);
  for (int f = 0; f < n_bins; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      data[f * n_frames + t] = std::complex<float>(f + 1.0f, t + 1.0f);
    }
  }

  Spectrogram spec =
      Spectrogram::from_complex(data.data(), n_bins, n_frames, n_fft, hop_length, sr);

  REQUIRE(spec.n_bins() == n_bins);
  REQUIRE(spec.n_frames() == n_frames);
  REQUIRE(spec.n_fft() == n_fft);
  REQUIRE(spec.hop_length() == hop_length);
  REQUIRE(spec.sample_rate() == sr);

  // Verify data
  REQUIRE(spec.at(0, 0) == std::complex<float>(1.0f, 1.0f));
  REQUIRE(spec.at(2, 1) == std::complex<float>(3.0f, 2.0f));
}
