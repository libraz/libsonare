/// @file onset_test.cpp
/// @brief Tests for onset strength functions.

#include "feature/onset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates a steady sine wave.
Audio create_steady_audio(float freq = 440.0f, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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
      samples[start + i] = envelope * std::sin(2.0f * sonare::constants::kPiD * 1000.0f * t);
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
  config2.detrend = false;

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
  with_detrend.detrend = true;

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

TEST_CASE("compute_onset_strength large-hop frame offset follows librosa floor", "[onset]") {
  // librosa computes the centered-frame offset with floor division. When
  // n_fft / (2 * hop_length) is 0.5, the offset is therefore zero.
  Audio audio = create_transient_audio(22050, 1.0f, 4);

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 512;
  mel_config.hop_length = 512;  // 2*hop (1024) > n_fft (512): floor(0.5) == 0

  OnsetConfig onset_config;  // center defaults to true
  onset_config.detrend = false;

  // The raw Mel overload remains unshifted because it has no FFT-size metadata.
  MelConfig aligned = mel_config;
  aligned.center = onset_config.center;
  MelSpectrogram mel = MelSpectrogram::compute(audio, aligned);
  std::vector<float> unshifted = compute_onset_strength(mel, onset_config);

  // The Audio overload supplies that metadata to the shared centering helper.
  std::vector<float> shifted = compute_onset_strength(audio, mel_config, onset_config);

  REQUIRE(shifted.size() == unshifted.size());
  REQUIRE(shifted.size() > 1);

  REQUIRE(shifted == unshifted);
}

TEST_CASE("compute_onset_strength default-config frame offset unchanged", "[onset]") {
  // Standard configs yield an integer quotient, so rounding must not alter the
  // shift: n_fft=2048, hop=512 -> exactly 2 frames (same as integer division).
  Audio audio = create_transient_audio(22050, 1.0f, 4);

  MelConfig mel_config;
  mel_config.n_mels = 40;
  mel_config.n_fft = 2048;
  mel_config.hop_length = 512;  // 2048/(2*512) = 2.0 exactly

  OnsetConfig onset_config;
  onset_config.detrend = false;

  MelConfig aligned = mel_config;
  aligned.center = onset_config.center;
  MelSpectrogram mel = MelSpectrogram::compute(audio, aligned);
  std::vector<float> unshifted = compute_onset_strength(mel, onset_config);
  std::vector<float> shifted = compute_onset_strength(audio, mel_config, onset_config);

  REQUIRE(shifted.size() == unshifted.size());
  REQUIRE(shifted.size() > 2);

  // Exactly a two-frame right shift: leading two frames zero, then aligned.
  REQUIRE(shifted[0] == 0.0f);
  REQUIRE(shifted[1] == 0.0f);
  for (size_t i = 2; i < shifted.size(); ++i) {
    REQUIRE(shifted[i] == unshifted[i - 2]);
  }

  REQUIRE(center_onset_strength(unshifted, mel_config.n_fft, mel_config.hop_length,
                                onset_config.center) == shifted);
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

namespace {

/// @brief spectral_flux() with the frame-major traversal it had before.
/// @details The inner loop over bins strides `magnitude` twice per frame. The library now
///          walks bins outside and accumulates per frame, which keeps each frame's bins in
///          ascending order -- what this oracle pins by exact comparison.
std::vector<float> oracle_spectral_flux(const std::vector<float>& magnitude, int n_bins,
                                        int n_frames, int lag) {
  std::vector<float> flux(static_cast<size_t>(n_frames), 0.0f);
  if (n_frames > lag) {
    const int diff_frames = n_frames - lag;
    for (int f = 0; f < diff_frames; ++f) {
      float sum = 0.0f;
      for (int b = 0; b < n_bins; ++b) {
        const float d = magnitude[static_cast<size_t>(b * n_frames + (f + lag))] -
                        magnitude[static_cast<size_t>(b * n_frames + f)];
        sum += std::abs(d);
      }
      flux[static_cast<size_t>(f + lag)] = sum;
    }
  }
  return flux;
}

}  // namespace

TEST_CASE("spectral_flux matches a frame-major oracle", "[onset]") {
  const Audio audio = create_transient_audio(22050, 1.0f, 6);
  StftConfig cfg;
  cfg.n_fft = 512;
  cfg.hop_length = 128;
  const Spectrogram spec = Spectrogram::compute(audio, cfg);
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  REQUIRE(n_bins > 0);
  REQUIRE(n_frames > 2);

  // lag 1 is the default; a larger lag shifts both reads within the row, which is where a
  // row-pointer hoist can go wrong without changing the shape of the result.
  for (int lag : {1, 3, 7}) {
    CAPTURE(lag);
    const std::vector<float> got = spectral_flux(spec, lag);
    const std::vector<float> want = oracle_spectral_flux(spec.magnitude(), n_bins, n_frames, lag);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); ++i) {
      CAPTURE(i, got[i], want[i]);
      REQUIRE(std::isfinite(got[i]));
      REQUIRE(got[i] == want[i]);
    }
  }
}

TEST_CASE("spectral_flux leaves every frame zero when the lag covers the signal", "[onset]") {
  // n_frames <= lag skips the accumulator entirely; the result must still be the documented
  // all-zero vector of the right length rather than an uninitialised scratch buffer.
  const Audio audio = create_steady_audio(440.0f, 22050, 0.05f);
  StftConfig cfg;
  cfg.n_fft = 512;
  cfg.hop_length = 128;
  const Spectrogram spec = Spectrogram::compute(audio, cfg);
  const std::vector<float> flux = spectral_flux(spec, spec.n_frames() + 1);
  REQUIRE(flux.size() == static_cast<size_t>(spec.n_frames()));
  for (float v : flux) REQUIRE(v == 0.0f);
}

TEST_CASE("splitting the Audio overload at the Mel spectrogram changes nothing", "[onset][reuse]") {
  // A caller that already holds an STFT of the same geometry can build the Mel spectrogram from
  // it and apply the alignment step itself instead of paying for a second STFT. That path has to
  // produce the identical envelope. The geometry has one owner because the Audio overload takes
  // its framing from onset_config.center rather than from the Mel config, so a shared
  // spectrogram has to carry that same centering.
  const Audio audio = create_transient_audio();

  MelConfig mel_config;
  mel_config.n_fft = 512;
  mel_config.hop_length = 128;
  OnsetConfig onset_config;
  onset_config.detrend = true;

  for (bool center : {true, false}) {
    CAPTURE(center);
    onset_config.center = center;

    StftConfig stft_config;
    stft_config.n_fft = mel_config.n_fft;
    stft_config.hop_length = mel_config.hop_length;
    stft_config.center = onset_config.center;
    const Spectrogram spec = Spectrogram::compute(audio, stft_config);
    const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, audio.sample_rate(),
                                                                mel_config.to_mel_filter_config());
    const std::vector<float> split =
        center_onset_strength(compute_onset_strength(mel, onset_config), stft_config.n_fft,
                              stft_config.hop_length, onset_config.center);

    const std::vector<float> whole = compute_onset_strength(audio, mel_config, onset_config);
    REQUIRE(split.size() == whole.size());
    REQUIRE_FALSE(whole.empty());
    for (size_t i = 0; i < whole.size(); ++i) {
      CAPTURE(i);
      REQUIRE(split[i] == whole[i]);
    }

    // The centering is not interchangeable: a spectrogram framed the other way produces a
    // different envelope, which is what makes sharing one config load-bearing rather than tidy.
    StftConfig mismatched_config = stft_config;
    mismatched_config.center = !onset_config.center;
    const Spectrogram mismatched_spec = Spectrogram::compute(audio, mismatched_config);
    const MelSpectrogram mismatched_mel = MelSpectrogram::from_spectrogram(
        mismatched_spec, audio.sample_rate(), mel_config.to_mel_filter_config());
    const std::vector<float> mismatched =
        center_onset_strength(compute_onset_strength(mismatched_mel, onset_config),
                              stft_config.n_fft, stft_config.hop_length, onset_config.center);
    REQUIRE(mismatched != whole);
  }
}
