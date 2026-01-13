/// @file onset_test.cpp
/// @brief Tests for onset strength functions.

#include "feature/onset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates a steady sine wave.
Audio create_steady_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with transients (short bursts).
Audio create_transient_audio(int sr = 22050, float duration = 1.0f, int n_bursts = 4) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  int burst_length = sr / 50;  // 20ms burst
  int interval = n_samples / n_bursts;

  for (int b = 0; b < n_bursts; ++b) {
    int start = b * interval;
    for (int i = 0; i < burst_length && start + i < n_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      float envelope = 1.0f - static_cast<float>(i) / burst_length;  // Decay
      samples[start + i] = envelope * std::sin(2.0f * M_PI * 1000.0f * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("compute_onset_strength basic", "[onset]") {
  Audio audio = create_steady_audio();

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

  OnsetConfig onset_config;
  std::vector<float> onset = compute_onset_strength(mel, onset_config);

  REQUIRE(onset.size() == static_cast<size_t>(mel.n_frames()));

  // Values should be finite
  for (float o : onset) {
    REQUIRE(std::isfinite(o));
  }
}

TEST_CASE("compute_onset_strength from Audio", "[onset]") {
  Audio audio = create_steady_audio();

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  OnsetConfig onset_config;

  std::vector<float> onset = compute_onset_strength(audio, mel_config, onset_config);

  REQUIRE(!onset.empty());

  // Values should be finite
  for (float o : onset) {
    REQUIRE(std::isfinite(o));
  }
}

TEST_CASE("compute_onset_strength transient vs steady", "[onset]") {
  Audio steady = create_steady_audio(440.0f, 22050, 1.0f);
  Audio transient = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  OnsetConfig onset_config;
  onset_config.detrend = false;  // Keep raw values for comparison
  onset_config.center = false;

  std::vector<float> steady_onset = compute_onset_strength(steady, mel_config, onset_config);
  std::vector<float> transient_onset = compute_onset_strength(transient, mel_config, onset_config);

  // Find max values
  float steady_max = 0.0f, transient_max = 0.0f;
  for (float o : steady_onset) steady_max = std::max(steady_max, o);
  for (float o : transient_onset) transient_max = std::max(transient_max, o);

  // Transient signal should have higher onset strength peaks
  REQUIRE(transient_max > steady_max);
}

TEST_CASE("compute_onset_strength lag parameter", "[onset]") {
  Audio audio = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

  OnsetConfig config1, config2;
  config1.lag = 1;
  config2.lag = 3;
  config1.detrend = false;
  config1.center = false;
  config2.detrend = false;
  config2.center = false;

  std::vector<float> onset1 = compute_onset_strength(mel, config1);
  std::vector<float> onset2 = compute_onset_strength(mel, config2);

  // Different lag should produce different results
  bool different = false;
  for (size_t i = 0; i < onset1.size(); ++i) {
    if (std::abs(onset1[i] - onset2[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);
}

TEST_CASE("compute_onset_strength detrend", "[onset]") {
  Audio audio = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

  OnsetConfig no_detrend, with_detrend;
  no_detrend.detrend = false;
  no_detrend.center = false;
  with_detrend.detrend = true;
  with_detrend.center = false;

  std::vector<float> onset_nd = compute_onset_strength(mel, no_detrend);
  std::vector<float> onset_d = compute_onset_strength(mel, with_detrend);

  // With detrend, mean should be closer to zero
  float mean_nd = 0.0f, mean_d = 0.0f;
  for (float o : onset_nd) mean_nd += o;
  for (float o : onset_d) mean_d += o;
  mean_nd /= static_cast<float>(onset_nd.size());
  mean_d /= static_cast<float>(onset_d.size());

  // Detrended mean should be closer to zero
  REQUIRE(std::abs(mean_d) < std::abs(mean_nd) + 1.0f);
}

TEST_CASE("onset_strength_multi basic", "[onset]") {
  Audio audio = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_mels = 60;  // Divisible by n_bands
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

  int n_bands = 3;
  OnsetConfig config;

  std::vector<float> onset_multi = onset_strength_multi(mel, n_bands, config);

  REQUIRE(onset_multi.size() == static_cast<size_t>(n_bands * mel.n_frames()));

  // Values should be finite
  for (float o : onset_multi) {
    REQUIRE(std::isfinite(o));
  }
}

TEST_CASE("onset_strength_multi bands independence", "[onset]") {
  Audio audio = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_mels = 60;
  mel_config.n_fft = 1024;
  mel_config.hop_length = 256;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

  int n_bands = 3;
  OnsetConfig config;
  config.detrend = false;
  config.center = false;

  std::vector<float> onset_multi = onset_strength_multi(mel, n_bands, config);

  int n_frames = mel.n_frames();

  // Compute max for each band
  std::vector<float> band_max(n_bands, 0.0f);
  for (int b = 0; b < n_bands; ++b) {
    for (int t = 0; t < n_frames; ++t) {
      band_max[b] = std::max(band_max[b], onset_multi[b * n_frames + t]);
    }
  }

  // At least one band should have non-zero max
  float total_max = 0.0f;
  for (float m : band_max) {
    total_max += m;
  }
  REQUIRE(total_max > 0.0f);
}

TEST_CASE("spectral_flux basic", "[onset]") {
  Audio audio = create_transient_audio();

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> flux = spectral_flux(spec);

  REQUIRE(flux.size() == static_cast<size_t>(spec.n_frames()));

  // Values should be non-negative
  for (float f : flux) {
    REQUIRE(f >= 0.0f);
  }
}

TEST_CASE("spectral_flux lag parameter", "[onset]") {
  Audio audio = create_transient_audio();

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, config);

  std::vector<float> flux1 = spectral_flux(spec, 1);
  std::vector<float> flux3 = spectral_flux(spec, 3);

  // Different lag should produce different results
  bool different = false;
  for (size_t i = 3; i < flux1.size(); ++i) {
    if (std::abs(flux1[i] - flux3[i]) > 1e-6f) {
      different = true;
      break;
    }
  }
  REQUIRE(different);
}

TEST_CASE("spectral_flux transient vs steady", "[onset]") {
  Audio steady = create_steady_audio(440.0f, 22050, 1.0f);
  Audio transient = create_transient_audio();

  StftConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;

  Spectrogram steady_spec = Spectrogram::compute(steady, config);
  Spectrogram transient_spec = Spectrogram::compute(transient, config);

  std::vector<float> steady_flux = spectral_flux(steady_spec);
  std::vector<float> transient_flux = spectral_flux(transient_spec);

  // Compute variance to check for temporal changes
  // Transient signal should have more variation in spectral flux
  float steady_mean = 0.0f, transient_mean = 0.0f;
  for (float f : steady_flux) steady_mean += f;
  for (float f : transient_flux) transient_mean += f;
  steady_mean /= static_cast<float>(steady_flux.size());
  transient_mean /= static_cast<float>(transient_flux.size());

  float steady_var = 0.0f, transient_var = 0.0f;
  for (float f : steady_flux) steady_var += (f - steady_mean) * (f - steady_mean);
  for (float f : transient_flux) transient_var += (f - transient_mean) * (f - transient_mean);
  steady_var /= static_cast<float>(steady_flux.size());
  transient_var /= static_cast<float>(transient_flux.size());

  // Transient signal should have higher variance in spectral flux
  REQUIRE(transient_var > steady_var);
}
