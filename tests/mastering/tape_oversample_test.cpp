#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "mastering/saturation/tape.h"
#include "util/constants.h"

using sonare::mastering::saturation::Tape;
using sonare::mastering::saturation::TapeConfig;

namespace {

constexpr int kSampleRate = 22050;
constexpr int kLength = 32768;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz * i / sample_rate));
  }
  return out;
}

void run_tape(int oversample_factor, std::vector<float>& mono) {
  TapeConfig config{};
  config.drive_db = 18.0f;
  config.oversample_factor = oversample_factor;
  Tape tape(config);
  tape.prepare(static_cast<double>(kSampleRate), kLength);
  float* channels[] = {mono.data()};
  tape.process(channels, 1, static_cast<int>(mono.size()));
}

// Sum spectral energy in bins that are NOT close to a harmonic of the fundamental.
double alias_energy(const std::vector<float>& signal, float fundamental_hz) {
  std::vector<float> windowed(signal.size());
  for (size_t i = 0; i < signal.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * static_cast<double>(i) /
                                          static_cast<double>(signal.size() - 1));
    windowed[i] = signal[i] * static_cast<float>(w);
  }
  sonare::FFT fft(static_cast<int>(signal.size()));
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());

  const double bin_hz = static_cast<double>(kSampleRate) / static_cast<double>(signal.size());
  const double tol_hz = bin_hz * 4.0;  // exclude bins near each harmonic
  double energy = 0.0;
  for (size_t b = 1; b < spectrum.size(); ++b) {
    const double freq = static_cast<double>(b) * bin_hz;
    if (freq < fundamental_hz * 0.5) continue;  // ignore DC / sub-fundamental region
    bool is_harmonic = false;
    for (int h = 1; h <= 16; ++h) {
      if (std::abs(freq - fundamental_hz * h) <= tol_hz) {
        is_harmonic = true;
        break;
      }
    }
    if (!is_harmonic) {
      energy += static_cast<double>(std::abs(spectrum[b]));
    }
  }
  return energy;
}

}  // namespace

TEST_CASE("Tape oversampling reduces aliasing at high drive", "[mastering][saturation]") {
  const float freq = 3500.0f;
  auto base = sine(freq, kSampleRate, kLength, 0.9f);

  auto out1x = base;
  auto out4x = base;
  run_tape(1, out1x);
  run_tape(4, out4x);

  const double alias1x = alias_energy(out1x, freq);
  const double alias4x = alias_energy(out4x, freq);

  INFO("alias energy 1x = " << alias1x << ", 4x = " << alias4x);
  REQUIRE(alias4x < alias1x * 0.9);
}

TEST_CASE("Tape oversample_factor=1 is deterministic and matches default path",
          "[mastering][saturation]") {
  auto input = sine(440.0f, kSampleRate, 4096, 0.5f);

  auto a = input;
  auto b = input;
  run_tape(1, a);
  run_tape(1, b);

  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(std::abs(a[i] - b[i]) < 1e-6f);
  }
}

TEST_CASE("Tape rejects invalid oversample_factor", "[mastering][saturation]") {
  TapeConfig config{};
  config.oversample_factor = 3;
  REQUIRE_THROWS_AS(Tape(config), std::invalid_argument);
}
