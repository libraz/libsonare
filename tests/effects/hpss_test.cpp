/// @file hpss_test.cpp
/// @brief Tests for HPSS (Harmonic-Percussive Source Separation).

#include "effects/hpss.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

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
    samples[i] = std::sin(2.0f * sonare::constants::kPiD * freq * t);
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
      samples[start + i] =
          envelope * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 2.0f - 1.0f);
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

TEST_CASE("hpss preserves spectrogram center flag", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;
  stft_config.center = false;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec);

  REQUIRE_FALSE(result.harmonic.center());
  REQUIRE_FALSE(result.percussive.center());
}

TEST_CASE("hpss preserves spectrogram win_length", "[hpss]") {
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.win_length = 1024;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec);

  REQUIRE(result.harmonic.win_length() == stft_config.win_length);
  REQUIRE(result.percussive.win_length() == stft_config.win_length);
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

  REQUIRE(result.harmonic.size() == audio.size());
  REQUIRE(result.percussive.size() == audio.size());
  REQUIRE(result.residual.size() == audio.size());
  double signal_energy = 0.0;
  double error_energy = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    const double reconstructed =
        static_cast<double>(result.harmonic[i]) + result.percussive[i] + result.residual[i];
    const double original = audio[i];
    signal_energy += original * original;
    const double error = original - reconstructed;
    error_energy += error * error;
  }
  REQUIRE(error_energy / signal_energy < 1.0e-8);
}

TEST_CASE("residual helper function", "[hpss]") {
  Audio audio = create_harmonic_audio();

  Audio res = residual(audio);

  REQUIRE(!res.empty());
  REQUIRE(res.sample_rate() == audio.sample_rate());
}

TEST_CASE("median_filter_horizontal handles NaN/Inf without UB", "[hpss][nan]") {
  // Inject NaN and Inf into a small magnitude grid. The sliding-window median
  // path used to rely on std::lower_bound + erase with floating-point equality,
  // which is UB once NaN enters the array. After the fix, NaN/Inf are sanitized
  // and the output must be fully finite.
  int n_bins = 4;
  int n_frames = 32;
  std::vector<float> input(n_bins * n_frames, 1.0f);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();

  // Sprinkle non-finite values across both boundary and middle regions.
  input[0 * n_frames + 0] = kNaN;
  input[0 * n_frames + 5] = kInf;
  input[1 * n_frames + 10] = -kInf;
  input[2 * n_frames + 15] = kNaN;
  input[3 * n_frames + n_frames - 1] = kNaN;

  std::vector<float> output = median_filter_horizontal(input.data(), n_bins, n_frames, 5);

  REQUIRE(output.size() == input.size());
  for (float v : output) {
    REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("median_filter_vertical handles NaN/Inf without UB", "[hpss][nan]") {
  int n_bins = 32;
  int n_frames = 4;
  std::vector<float> input(n_bins * n_frames, 1.0f);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();

  input[0 * n_frames + 0] = kNaN;
  input[7 * n_frames + 1] = kInf;
  input[15 * n_frames + 2] = -kInf;
  input[(n_bins - 1) * n_frames + 3] = kNaN;

  std::vector<float> output = median_filter_vertical(input.data(), n_bins, n_frames, 5);

  REQUIRE(output.size() == input.size());
  for (float v : output) {
    REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("hpss spectrogram with NaN does not propagate NaN", "[hpss][nan]") {
  // Build a real spectrogram, then poison a few complex bins with NaN.
  // After the SlidingMedian fix, the result should be finite everywhere and
  // hpss() should not throw.
  Audio audio = create_harmonic_audio();

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram clean = Spectrogram::compute(audio, stft_config);
  int n_bins = clean.n_bins();
  int n_frames = clean.n_frames();
  REQUIRE(n_bins > 0);
  REQUIRE(n_frames > 0);

  std::vector<std::complex<float>> poisoned(clean.complex_data(),
                                            clean.complex_data() + n_bins * n_frames);
  const float kNaN = std::numeric_limits<float>::quiet_NaN();
  const float kInf = std::numeric_limits<float>::infinity();
  // Poison a handful of bins across the time axis (boundary + middle).
  poisoned[0] = std::complex<float>(kNaN, 0.0f);
  poisoned[(n_bins / 2) * n_frames + n_frames / 2] = std::complex<float>(kInf, kNaN);
  poisoned[(n_bins - 1) * n_frames + (n_frames - 1)] = std::complex<float>(0.0f, kNaN);

  Spectrogram spec = Spectrogram::from_complex(poisoned.data(), n_bins, n_frames, clean.n_fft(),
                                               clean.hop_length(), clean.sample_rate(),
                                               clean.window(), clean.center(), clean.win_length());

  HpssConfig config;
  config.kernel_size_harmonic = 11;
  config.kernel_size_percussive = 11;

  HpssSpectrogramResult result;
  REQUIRE_NOTHROW(result = hpss(spec, config));

  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();
  REQUIRE(h_mag.size() == static_cast<size_t>(n_bins * n_frames));
  REQUIRE(p_mag.size() == static_cast<size_t>(n_bins * n_frames));

  // NaN may remain wherever the poisoned bin was multiplied by the mask,
  // but the median-derived masks themselves must never produce NaN. Count
  // non-finite outputs to ensure we are not catastrophically propagating.
  int nan_count = 0;
  for (float v : h_mag) {
    if (!std::isfinite(v)) ++nan_count;
  }
  for (float v : p_mag) {
    if (!std::isfinite(v)) ++nan_count;
  }
  // At most a small handful (one per poisoned bin per side) is acceptable.
  REQUIRE(nan_count <= 8);
}

TEST_CASE("hpss separates pure sine into mostly harmonic", "[hpss][separation]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 1.0f);

  HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec, config);

  // Sum magnitudes; harmonic energy should dominate for a pure sine wave.
  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();

  double h_energy = 0.0;
  double p_energy = 0.0;
  for (float v : h_mag) h_energy += static_cast<double>(v) * static_cast<double>(v);
  for (float v : p_mag) p_energy += static_cast<double>(v) * static_cast<double>(v);

  REQUIRE(h_energy > 0.0);
  // Harmonic energy should be at least a few times larger than percussive.
  REQUIRE(h_energy > 3.0 * p_energy);
}

TEST_CASE("hpss separates impulses into mostly percussive", "[hpss][separation]") {
  // Single sharp impulse: broadband, transient -> percussive should dominate.
  int sr = 22050;
  int n_samples = sr;  // 1 second
  std::vector<float> samples(n_samples, 0.0f);
  // Place a couple of broadband clicks far apart so they look transient
  // relative to the analysis frame structure.
  samples[sr / 4] = 1.0f;
  samples[sr / 2] = 1.0f;
  samples[3 * sr / 4] = 1.0f;
  Audio audio = Audio::from_vector(std::move(samples), sr);

  HpssConfig config;
  config.kernel_size_harmonic = 31;
  config.kernel_size_percussive = 31;

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec, config);

  const std::vector<float>& h_mag = result.harmonic.magnitude();
  const std::vector<float>& p_mag = result.percussive.magnitude();

  double h_energy = 0.0;
  double p_energy = 0.0;
  for (float v : h_mag) h_energy += static_cast<double>(v) * static_cast<double>(v);
  for (float v : p_mag) p_energy += static_cast<double>(v) * static_cast<double>(v);

  REQUIRE(p_energy > 0.0);
  // Percussive energy should clearly exceed harmonic for sparse impulses.
  REQUIRE(p_energy > h_energy);
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

// Regression for the soft-mask margin-domain bug. librosa applies the margin
// *before* the power: mask_harm = H^p / (H^p + (margin_h * P)^p), so the margin
// contributes margin^power. The previous code multiplied by the margin *after*
// the power (contributing only margin^1).
//
// With a uniform-magnitude spectrogram, both the horizontal and vertical median
// filters return that same constant, so at interior bins H == P. The harmonic
// mask there reduces to 1 / (1 + margin_h^power), which is directly observable
// as (harmonic output magnitude) / (input magnitude).
TEST_CASE("HPSS soft mask applies margin before the power (librosa parity)", "[hpss]") {
  constexpr int kBins = 9;
  constexpr int kFrames = 9;
  constexpr float kAmp = 1.0f;
  std::vector<std::complex<float>> data(static_cast<size_t>(kBins * kFrames),
                                        std::complex<float>(kAmp, 0.0f));
  Spectrogram spec =
      Spectrogram::from_complex(data.data(), kBins, kFrames, /*n_fft=*/16,
                                /*hop_length=*/8, /*sample_rate=*/22050, WindowType::Hann);

  HpssConfig config;
  config.use_soft_mask = true;
  config.power = 2.0f;
  config.margin_harmonic = 3.0f;
  config.margin_percussive = 3.0f;
  // Keep kernels small so the interior stays uniform under both median filters.
  config.kernel_size_harmonic = 3;
  config.kernel_size_percussive = 3;

  HpssSpectrogramResult result = hpss(spec, config);

  // Interior bin/frame, away from median-filter edge effects.
  const int idx = 4 * kFrames + 4;
  const float harm_mag = result.harmonic.magnitude()[static_cast<size_t>(idx)];
  const float perc_mag = result.percussive.magnitude()[static_cast<size_t>(idx)];

  // Corrected (librosa) mask: margin contributes margin^power.
  const float expected = 1.0f / (1.0f + std::pow(config.margin_harmonic, config.power));  // 1/10
  // The buggy formula applied the margin after the power -> 1 / (1 + margin).
  const float buggy = 1.0f / (1.0f + config.margin_harmonic);  // 1/4

  REQUIRE_THAT(harm_mag, WithinAbs(kAmp * expected, 1e-4f));
  REQUIRE_THAT(perc_mag, WithinAbs(kAmp * expected, 1e-4f));
  // Guard that the assertion would actually catch the old behavior.
  REQUIRE(std::abs(harm_mag - kAmp * buggy) > 0.1f);
}

// Asymmetric margins must follow the per-side margin^power law independently.
TEST_CASE("HPSS soft mask honors asymmetric margins with margin^power", "[hpss]") {
  constexpr int kBins = 9;
  constexpr int kFrames = 9;
  std::vector<std::complex<float>> data(static_cast<size_t>(kBins * kFrames),
                                        std::complex<float>(1.0f, 0.0f));
  Spectrogram spec =
      Spectrogram::from_complex(data.data(), kBins, kFrames, /*n_fft=*/16,
                                /*hop_length=*/8, /*sample_rate=*/22050, WindowType::Hann);

  HpssConfig config;
  config.use_soft_mask = true;
  config.power = 2.0f;
  config.margin_harmonic = 2.0f;
  config.margin_percussive = 4.0f;
  config.kernel_size_harmonic = 3;
  config.kernel_size_percussive = 3;

  HpssSpectrogramResult result = hpss(spec, config);

  const int idx = 4 * kFrames + 4;
  const float harm_mag = result.harmonic.magnitude()[static_cast<size_t>(idx)];
  const float perc_mag = result.percussive.magnitude()[static_cast<size_t>(idx)];

  const float expected_h = 1.0f / (1.0f + std::pow(config.margin_harmonic, config.power));  // 1/5
  const float expected_p =
      1.0f / (1.0f + std::pow(config.margin_percussive, config.power));  // 1/17

  REQUIRE_THAT(harm_mag, WithinAbs(expected_h, 1e-4f));
  REQUIRE_THAT(perc_mag, WithinAbs(expected_p, 1e-4f));
}

TEST_CASE("hpss resynthesis reconstructs the input for every analysis window", "[hpss]") {
  // The separated components are resynthesized through the iSTFT, whose
  // overlap-add divisor has to be built from the analysis window. If it is not,
  // the harmonic and percussive signals each carry a gain ripple at the hop
  // rate and their sum no longer returns the input, however exact the masking.
  constexpr int sr = 22050;
  constexpr int samples = sr / 2;
  std::vector<float> source(samples);
  for (int i = 0; i < samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    source[i] = 0.4f * std::sin(2.0f * static_cast<float>(constants::kPiD) * 220.0f * t);
    if (i % 2205 == 0) {
      source[i] += 0.5f;
    }
  }
  const Audio audio = Audio::from_vector(std::vector<float>(source), sr);

  const auto relative_rms_error = [&source](const std::vector<float>& sum, size_t skip) {
    double num = 0.0;
    double den = 0.0;
    for (size_t i = skip; i + skip < source.size(); ++i) {
      const double diff = static_cast<double>(source[i]) - static_cast<double>(sum[i]);
      num += diff * diff;
      den += static_cast<double>(source[i]) * static_cast<double>(source[i]);
    }
    REQUIRE(den > 0.0);
    return std::sqrt(num / den);
  };

  for (const WindowType window :
       {WindowType::Hann, WindowType::Hamming, WindowType::Blackman, WindowType::Rectangular}) {
    CAPTURE(static_cast<int>(window));
    StftConfig stft_config;
    stft_config.n_fft = 2048;
    stft_config.hop_length = 512;
    stft_config.window = window;
    const HpssConfig config;
    const size_t skip = static_cast<size_t>(stft_config.n_fft);

    SECTION("harmonic plus percussive") {
      const HpssAudioResult result = hpss(audio, config, stft_config);
      REQUIRE(result.harmonic.size() == source.size());
      REQUIRE(result.percussive.size() == source.size());
      std::vector<float> sum(source.size());
      for (size_t i = 0; i < source.size(); ++i) {
        sum[i] = result.harmonic.data()[i] + result.percussive.data()[i];
      }
      REQUIRE(relative_rms_error(sum, skip) < 1e-5);
    }

    SECTION("harmonic plus percussive plus residual") {
      const HpssAudioResultWithResidual result = hpss_with_residual(audio, config, stft_config);
      REQUIRE(result.harmonic.size() == source.size());
      std::vector<float> sum(source.size());
      for (size_t i = 0; i < source.size(); ++i) {
        sum[i] =
            result.harmonic.data()[i] + result.percussive.data()[i] + result.residual.data()[i];
      }
      REQUIRE(relative_rms_error(sum, skip) < 1e-5);
    }
  }
}

TEST_CASE("hpss rejects a hop below the half-window overlap contract", "[hpss]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.25f);
  HpssConfig config;
  config.kernel_size_harmonic = 7;
  config.kernel_size_percussive = 7;

  StftConfig no_overlap;
  no_overlap.n_fft = 1024;
  no_overlap.hop_length = 1024;
  REQUIRE_THROWS_AS(hpss(audio, config, no_overlap), SonareException);
  REQUIRE_THROWS_AS(hpss_with_residual(audio, config, no_overlap), SonareException);

  StftConfig sparse;
  sparse.n_fft = 512;
  sparse.hop_length = 2048;
  REQUIRE_THROWS_AS(hpss(audio, config, sparse), SonareException);
  REQUIRE_THROWS_AS(hpss_with_residual(audio, config, sparse), SonareException);

  StftConfig ok = no_overlap;
  ok.hop_length = 512;
  REQUIRE_NOTHROW(hpss(audio, config, ok));
}

TEST_CASE("hpss accepts an even n_fft that is not a power of two", "[hpss]") {
  Audio audio = create_harmonic_audio(440.0f, 22050, 0.25f);
  HpssConfig config;
  config.kernel_size_harmonic = 7;
  config.kernel_size_percussive = 7;

  StftConfig stft_config;
  stft_config.n_fft = 1500;
  stft_config.hop_length = 250;

  const HpssAudioResult result = hpss(audio, config, stft_config);
  REQUIRE(result.harmonic.size() == audio.size());
  REQUIRE(result.percussive.size() == audio.size());
  for (size_t i = 0; i < result.harmonic.size(); ++i) {
    REQUIRE(std::isfinite(result.harmonic.data()[i]));
    REQUIRE(std::isfinite(result.percussive.data()[i]));
  }
}
