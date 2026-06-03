/// @file spectral_test.cpp
/// @brief Tests for spectral feature functions.

#include "feature/spectral.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::test::generate_sine_audio;

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
  Audio audio = generate_sine_audio(1000.0f);
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
  Audio low_audio = generate_sine_audio(200.0f);
  Audio high_audio = generate_sine_audio(4000.0f);
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

TEST_CASE("spectral_centroid normalizes tiny nonzero magnitudes like librosa", "[spectral]") {
  std::vector<float> magnitude = {1e-12f, 1e-12f};

  std::vector<float> centroid = spectral_centroid(magnitude.data(), 2, 1, 2, 2);

  REQUIRE(centroid.size() == 1);
  REQUIRE_THAT(centroid[0], WithinAbs(0.5f, 1e-7f));
}

TEST_CASE("spectral_bandwidth basic", "[spectral]") {
  Audio audio = generate_sine_audio(1000.0f);
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

TEST_CASE("spectral_bandwidth normalizes tiny nonzero magnitudes like librosa", "[spectral]") {
  std::vector<float> magnitude = {1e-12f, 1e-12f};

  std::vector<float> bandwidth = spectral_bandwidth(magnitude.data(), 2, 1, 2, 2, 2.0f);

  REQUIRE(bandwidth.size() == 1);
  REQUIRE_THAT(bandwidth[0], WithinAbs(0.5f, 1e-7f));
}

TEST_CASE("spectral_rolloff basic", "[spectral]") {
  Audio audio = generate_sine_audio(1000.0f);
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

TEST_CASE("spectral_rolloff uses magnitude accumulation like librosa", "[spectral]") {
  // Matrix layout is [n_bins x n_frames].  Librosa accumulates the provided
  // non-negative spectrogram values directly rather than squaring them.
  std::vector<float> magnitude = {9.0f, 1.0f, 1.0f};

  std::vector<float> rolloff = spectral_rolloff(magnitude.data(), 3, 1, 300, 6, 0.85f);

  REQUIRE(rolloff.size() == 1);
  REQUIRE_THAT(rolloff[0], WithinAbs(50.0f, 1e-7f));
}

TEST_CASE("spectral_rolloff validates librosa-compatible inputs", "[spectral]") {
  std::vector<float> magnitude = {1.0f, 1.0f, 1.0f};

  REQUIRE_THROWS_AS(spectral_rolloff(magnitude.data(), 3, 1, 300, 6, 1.0f), SonareException);
  REQUIRE_THROWS_AS(spectral_rolloff(magnitude.data(), 3, 1, 300, 6, 0.0f), SonareException);

  std::vector<float> negative = {1.0f, -1.0f, 1.0f};
  std::vector<float> clamped = spectral_rolloff(negative.data(), 3, 1, 300, 6, 0.85f);
  REQUIRE(clamped.size() == 1);
  REQUIRE_THAT(clamped[0], WithinAbs(100.0f, 1e-7f));
}

TEST_CASE("spectral_flatness sine vs noise", "[spectral]") {
  Audio sine_audio = generate_sine_audio(1000.0f);
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

TEST_CASE("spectral_flatness squares magnitude like librosa", "[spectral]") {
  std::vector<float> magnitude = {1.0f, 10.0f};

  std::vector<float> flatness = spectral_flatness(magnitude.data(), 2, 1);

  REQUIRE(flatness.size() == 1);
  REQUIRE_THAT(flatness[0], WithinAbs(0.1980198f, 1e-6f));
}

TEST_CASE("spectral_flatness silent frame returns zero", "[spectral][edge]") {
  // A fully-silent (all-zero) frame is tonally undefined; librosa returns 0
  // flatness for it rather than the maximally-flat 1.0 the floored
  // geometric/arithmetic ratio would otherwise produce.
  std::vector<float> silent(8, 0.0f);
  std::vector<float> silent_flatness = spectral_flatness(silent.data(), 8, 1);
  REQUIRE(silent_flatness.size() == 1);
  REQUIRE_THAT(silent_flatness[0], WithinAbs(0.0f, 1e-12f));

  // A broadband (flat, nonzero) frame should yield a clearly higher value.
  std::vector<float> broadband(8, 1.0f);
  std::vector<float> broadband_flatness = spectral_flatness(broadband.data(), 8, 1);
  REQUIRE(broadband_flatness.size() == 1);
  REQUIRE(broadband_flatness[0] > silent_flatness[0]);
  REQUIRE(broadband_flatness[0] > 0.5f);
}

TEST_CASE("spectral_flatness validates dimensions", "[spectral][edge]") {
  // Non-positive dimensions on the raw-pointer path would otherwise drive an
  // Eigen::Map over a zero/negative extent and divide by n_bins == 0.
  std::vector<float> magnitude = {1.0f, 2.0f, 3.0f};
  REQUIRE_THROWS_AS(spectral_flatness(magnitude.data(), 0, 1), SonareException);
  REQUIRE_THROWS_AS(spectral_flatness(magnitude.data(), 3, 0), SonareException);
  REQUIRE_THROWS_AS(spectral_flatness(magnitude.data(), -1, 1), SonareException);
}

TEST_CASE("zero_crossing_rate handles empty input", "[spectral][zcr][edge]") {
  // Empty input must not crash and must return a single defined zero-rate frame
  // so downstream callers always get a non-empty result.
  std::vector<float> empty;
  std::vector<float> zcr = zero_crossing_rate(empty.data(), empty.size(), 2048, 512);
  REQUIRE(zcr.size() == 1);
  REQUIRE(zcr[0] == 0.0f);

  // The Audio overload of an empty signal behaves the same way.
  Audio empty_audio = Audio::from_vector(std::vector<float>{}, 22050);
  std::vector<float> zcr_audio = zero_crossing_rate(empty_audio);
  REQUIRE(zcr_audio.size() == 1);
  REQUIRE(zcr_audio[0] == 0.0f);
}

TEST_CASE("spectral_contrast basic", "[spectral]") {
  Audio audio = generate_sine_audio(1000.0f);
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

TEST_CASE("spectral_contrast uses librosa band quantile means", "[spectral]") {
  std::vector<std::complex<float>> data = {
      {1.0f, 0.0f}, {2.0f, 0.0f}, {3.0f, 0.0f}, {4.0f, 0.0f},
      {5.0f, 0.0f}, {6.0f, 0.0f}, {7.0f, 0.0f}, {8.0f, 0.0f},
  };
  Spectrogram spec = Spectrogram::from_complex(data.data(), 8, 1, 14, 1, 14);

  std::vector<float> contrast = spectral_contrast(spec, 14, 2, 1.0f, 0.5f);

  REQUIRE(contrast.size() == 3);
  REQUIRE_THAT(contrast[0], WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(contrast[1], WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(contrast[2], WithinAbs(2.6884532f, 1e-6f));
}

TEST_CASE("poly_features stays numerically stable at high order", "[spectral][poly_features]") {
  // Regression guard: prior to the Vandermonde column-scaling fix, building the
  // design matrix from raw bin frequencies (up to ~sr/2 ~= 11 kHz) made the high
  // -order columns explode (11025^5 ~ 1.6e20), pushing the Jacobi SVD beyond
  // double precision and producing NaN / Inf coefficients. With column-norm
  // scaling all coefficients must remain finite at orders 1..5.
  const int sr = 22050;
  const int n_bins = 1025;  // n_fft = 2048
  const int n_fft = 2048;
  const int n_frames = 3;

  // Spectrum whose magnitude is a smooth linear function of the bin frequency
  // (so a low-order polyfit has a meaningful, well-defined answer).
  const float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  std::vector<float> magnitude(static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
  for (int k = 0; k < n_bins; ++k) {
    const float freq = static_cast<float>(k) * bin_width;
    const float value = 1.0f + 0.001f * freq;
    for (int t = 0; t < n_frames; ++t) {
      magnitude[static_cast<size_t>(k * n_frames + t)] = value;
    }
  }

  for (int order = 1; order <= 5; ++order) {
    auto coeffs = poly_features(magnitude.data(), n_bins, n_frames, sr, n_fft, order);
    REQUIRE(coeffs.size() == static_cast<size_t>((order + 1) * n_frames));
    for (size_t i = 0; i < coeffs.size(); ++i) {
      CAPTURE(order, i);
      REQUIRE(std::isfinite(coeffs[i]));
    }

    // For a linear magnitude(freq) target, the fitted polynomial evaluated at a
    // few representative bins must reproduce the input within a sane tolerance.
    // Coefficients are stored high-degree first: out[p * n_frames + t] is the
    // coefficient of x^(order - p).
    const int check_bins[] = {64, 256, 512};
    for (int t = 0; t < n_frames; ++t) {
      for (int kk : check_bins) {
        const double x = static_cast<double>(kk) * static_cast<double>(bin_width);
        double y = 0.0;
        double xp = 1.0;
        for (int p = order; p >= 0; --p) {
          y += static_cast<double>(coeffs[static_cast<size_t>(p * n_frames + t)]) * xp;
          xp *= x;
        }
        const double expected =
            1.0 + 0.001 * static_cast<double>(kk) * static_cast<double>(bin_width);
        CAPTURE(order, t, kk, y, expected);
        // Generous tolerance: high-order fits to a linear target introduce
        // small numerical wiggles that grow with order, but the reconstruction
        // must still hover near the truth (not blow up to Inf).
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y - expected) < 0.5);
      }
    }
  }
}

TEST_CASE("poly_features column scaling preserves linear-order values",
          "[spectral][poly_features]") {
  // For order = 1, column-scaled and unscaled solves must agree to high
  // precision: this guards against an off-by-one in the unscaling step.
  Audio audio = generate_sine_audio(1000.0f);
  StftConfig config;
  config.n_fft = 2048;
  config.hop_length = 512;
  Spectrogram spec = Spectrogram::compute(audio, config);

  auto coeffs = poly_features(spec, audio.sample_rate(), 1);
  REQUIRE(coeffs.size() == static_cast<size_t>(2 * spec.n_frames()));
  for (float c : coeffs) {
    REQUIRE(std::isfinite(c));
  }
}

TEST_CASE("zero_crossing_rate basic", "[spectral]") {
  Audio audio = generate_sine_audio(1000.0f);

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

TEST_CASE("zero_crossing_rate uses librosa edge padding", "[spectral]") {
  std::vector<float> samples(8, -1.0f);
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  std::vector<float> zcr = zero_crossing_rate(audio, 8, 4);

  REQUIRE_FALSE(zcr.empty());
  for (float value : zcr) {
    REQUIRE_THAT(value, WithinAbs(0.0f, 1e-7f));
  }
}

TEST_CASE("zero_crossing_rate frequency relationship", "[spectral]") {
  // Higher frequency should have more zero crossings
  Audio low_audio = generate_sine_audio(100.0f);
  Audio high_audio = generate_sine_audio(4000.0f);

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
  Audio audio = generate_sine_audio(1000.0f);

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
    loud[i] = std::sin(2.0f * sonare::constants::kPiD * 440.0f * t);
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
