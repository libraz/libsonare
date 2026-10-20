#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "mastering/final/dither.h"
#include "mastering/final/noise_shaping.h"
#include "mastering/saturation/bitcrusher.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::mastering::final;
using sonare::constants::kTwoPiD;

namespace {

constexpr std::size_t kSamples = std::size_t{1} << 17;
constexpr int kFftSize = 2048;
constexpr int kHop = kFftSize / 2;
constexpr int kTargetBits = 16;
constexpr uint32_t kSeed = 0x2468ACEu;
constexpr double kReferenceRate = 44100.0;

struct NoiseSpectrum {
  int sample_rate;
  std::vector<double> power;  // per FFT bin, as a multiple of the quantizer's white error

  double band(double lo_hz, double hi_hz) const {
    const double bin_hz = static_cast<double>(sample_rate) / kFftSize;
    double sum = 0.0;
    int count = 0;
    for (std::size_t bin = 0; bin < power.size(); ++bin) {
      const double hz = static_cast<double>(bin) * bin_hz;
      if (hz < lo_hz || hz >= hi_hz) continue;
      sum += power[bin];
      ++count;
    }
    REQUIRE(count > 0);
    return sum / count;
  }
};

// Welch spectrum of noise-shaped quantization of silence, given in quantizer
// steps, divided by the 1/4 step^2 white total error (rounding plus TPDF) it is
// shaped from, so a bin reads |E(f)|^2 of the error-feedback loop that ran.
NoiseSpectrum shaped_noise_spectrum(const std::vector<float>& shaped, int sample_rate) {
  std::vector<float> window(kFftSize);
  double window_energy = 0.0;
  for (int n = 0; n < kFftSize; ++n) {
    window[n] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPiD * n / kFftSize));
    window_energy += static_cast<double>(window[n]) * window[n];
  }

  FFT fft(kFftSize);
  std::vector<float> frame(kFftSize);
  std::vector<std::complex<float>> spectrum(fft.n_bins());
  NoiseSpectrum result{sample_rate, std::vector<double>(fft.n_bins(), 0.0)};
  int frames = 0;
  for (std::size_t start = 0; start + kFftSize <= shaped.size(); start += kHop) {
    for (int n = 0; n < kFftSize; ++n) frame[n] = shaped[start + n] * window[n];
    fft.forward(frame.data(), spectrum.data());
    for (int bin = 0; bin < fft.n_bins(); ++bin) result.power[bin] += std::norm(spectrum[bin]);
    ++frames;
  }
  const double white_error = window_energy / 4.0 * frames;
  for (double& value : result.power) value /= white_error;
  return result;
}

NoiseSpectrum dither_noise_spectrum(int sample_rate) {
  const std::vector<float> silence(kSamples, 0.0f);
  const Audio output = dither(Audio::from_buffer(silence.data(), silence.size(), sample_rate),
                              {DitherType::NoiseShaped, kTargetBits, kSeed});
  REQUIRE(output.size() == kSamples);
  const float steps_per_unit = static_cast<float>(int64_t{1} << (kTargetBits - 1));
  std::vector<float> steps(kSamples);
  for (std::size_t i = 0; i < kSamples; ++i) steps[i] = output[i] * steps_per_unit;
  return shaped_noise_spectrum(steps, sample_rate);
}

NoiseSpectrum bitcrusher_noise_spectrum(int sample_rate, int downsample_factor) {
  mastering::saturation::BitCrusherConfig config;
  config.bit_depth = kTargetBits;
  config.downsample_factor = downsample_factor;
  config.dither_type = DitherType::NoiseShaped;
  config.dither_seed = kSeed;
  mastering::saturation::BitCrusher crusher(config);
  crusher.prepare(static_cast<double>(sample_rate), static_cast<int>(kSamples));
  std::vector<float> samples(kSamples, 0.0f);
  float* channels[] = {samples.data()};
  crusher.process(channels, 1, static_cast<int>(kSamples));
  const float steps_per_unit = static_cast<float>((int64_t{1} << (kTargetBits - 1)) - 1);
  for (float& sample : samples) sample *= steps_per_unit;
  return shaped_noise_spectrum(samples, sample_rate);
}

// Mean of the published |E(f)|^2 over the same bins NoiseSpectrum::band averages.
double published_band(double lo_hz, double hi_hz) {
  const double bin_hz = kReferenceRate / kFftSize;
  double sum = 0.0;
  int count = 0;
  for (int bin = 0; bin <= kFftSize / 2; ++bin) {
    const double hz = bin * bin_hz;
    if (hz < lo_hz || hz >= hi_hz) continue;
    sum += noise_shaping_detail::published_noise_power(hz);
    ++count;
  }
  return sum / count;
}

double db(double ratio) { return 10.0 * std::log10(ratio); }

// Rise of the noise from the 1-5 kHz minimum to the 14-16 kHz shoulder, in dB.
double shoulder_over_minimum_db(const NoiseSpectrum& spectrum) {
  return db(spectrum.band(14000.0, 16000.0) / spectrum.band(2500.0, 3500.0));
}

// Centre of the quietest 500 Hz band between 0.5 and 10 kHz.
double quietest_band_hz(const NoiseSpectrum& spectrum) {
  double quietest_power = 0.0;
  double quietest_hz = 0.0;
  for (double lo = 500.0; lo < 10000.0; lo += 500.0) {
    const double power = spectrum.band(lo, lo + 500.0);
    if (quietest_hz == 0.0 || power < quietest_power) {
      quietest_power = power;
      quietest_hz = lo + 250.0;
    }
  }
  return quietest_hz;
}

}  // namespace

TEST_CASE("Noise-shaped dither at 44.1 kHz realizes the published filter", "[mastering][final]") {
  const NoiseSpectrum spectrum = dither_noise_spectrum(static_cast<int>(kReferenceRate));
  // Each band averages a few thousand periodogram bins, so its estimate is good
  // to about 0.1 dB; a coefficient off by 1e-3 moves the 3 kHz minimum by ~0.5 dB.
  for (double lo = 1000.0; lo < 20000.0; lo += 1000.0) {
    const double measured_db = db(spectrum.band(lo, lo + 1000.0));
    const double published_db = db(published_band(lo, lo + 1000.0));
    CAPTURE(lo, measured_db, published_db);
    REQUIRE(std::abs(measured_db - published_db) <= 0.5);
  }
}

TEST_CASE("Noise-shaped dither keeps the 44.1 kHz noise curve in hertz at other rates",
          "[mastering][final]") {
  const int sample_rate = GENERATE(48000, 96000);
  const NoiseSpectrum reference = dither_noise_spectrum(static_cast<int>(kReferenceRate));
  const NoiseSpectrum spectrum = dither_noise_spectrum(sample_rate);

  const double reference_rise = shoulder_over_minimum_db(reference);
  const double rise = shoulder_over_minimum_db(spectrum);
  const double quietest_hz = quietest_band_hz(spectrum);
  CAPTURE(sample_rate, reference_rise, rise, quietest_hz);
  // The order-9 refit lands about 3 dB short of the 31 dB reference rise at
  // 96 kHz; the 44.1 kHz coefficients reused unchanged fall 10 dB short at 48 kHz
  // and 22 dB short at 96 kHz.
  REQUIRE(std::abs(rise - reference_rise) <= 4.0);
  REQUIRE(quietest_hz >= 2000.0);
  REQUIRE(quietest_hz <= 5000.0);
}

TEST_CASE("BitCrusher noise shaping follows the 44.1 kHz curve at its latch rate",
          "[mastering][saturation]") {
  // At 96 kHz a factor of 2 latches at 48 kHz, which the design has to follow:
  // designed at the host rate instead, the rise overshoots by 6 dB and the
  // quietest band drops to 1.75 kHz.
  const auto [sample_rate, downsample_factor] =
      GENERATE(std::make_pair(44100, 1), std::make_pair(96000, 1), std::make_pair(96000, 2));
  const NoiseSpectrum reference = dither_noise_spectrum(static_cast<int>(kReferenceRate));
  const NoiseSpectrum spectrum = bitcrusher_noise_spectrum(sample_rate, downsample_factor);

  const double reference_rise = shoulder_over_minimum_db(reference);
  const double rise = shoulder_over_minimum_db(spectrum);
  const double quietest_hz = quietest_band_hz(spectrum);
  CAPTURE(sample_rate, downsample_factor, reference_rise, rise, quietest_hz);
  REQUIRE(std::abs(rise - reference_rise) <= 4.0);
  REQUIRE(quietest_hz >= 2000.0);
  REQUIRE(quietest_hz <= 5000.0);
}
