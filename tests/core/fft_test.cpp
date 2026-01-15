/// @file fft_test.cpp
/// @brief Tests for FFT wrapper.

#include "core/fft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
}  // namespace

TEST_CASE("FFT forward / inverse roundtrip", "[fft]") {
  constexpr int n_fft = 64;
  FFT fft(n_fft);

  // Test signal: sine wave at bin 4
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = std::sin(kTwoPi * 4 * i / n_fft);
  }

  // Forward FFT
  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  // Expect peak at bin 4
  float max_mag = 0;
  int max_bin = 0;
  for (int i = 0; i < fft.n_bins(); ++i) {
    float mag = std::abs(spectrum[i]);
    if (mag > max_mag) {
      max_mag = mag;
      max_bin = i;
    }
  }
  REQUIRE(max_bin == 4);

  // Inverse FFT
  std::vector<float> output(n_fft);
  fft.inverse(spectrum.data(), output.data());

  // Output should match input
  for (int i = 0; i < n_fft; ++i) {
    REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-5f));
  }
}

TEST_CASE("FFT DC component", "[fft]") {
  constexpr int n_fft = 8;
  FFT fft(n_fft);

  // DC signal (all 1.0)
  std::vector<float> input(n_fft, 1.0f);
  std::vector<std::complex<float>> spectrum(fft.n_bins());

  fft.forward(input.data(), spectrum.data());

  // Bin 0 (DC) should have all energy
  REQUIRE_THAT(std::abs(spectrum[0]), WithinAbs(8.0f, 1e-5f));
  for (int i = 1; i < fft.n_bins(); ++i) {
    REQUIRE_THAT(std::abs(spectrum[i]), WithinAbs(0.0f, 1e-5f));
  }
}

TEST_CASE("FFT non-power-of-2 sizes", "[fft]") {
  // KissFFT supports non-power-of-2 sizes, but they may be slower
  SECTION("n_fft = 100") {
    FFT fft(100);
    REQUIRE(fft.n_fft() == 100);
    REQUIRE(fft.n_bins() == 51);  // 100/2 + 1

    std::vector<float> input(100, 0.0f);
    input[0] = 1.0f;  // Impulse
    std::vector<std::complex<float>> spectrum(51);
    fft.forward(input.data(), spectrum.data());

    // Impulse has flat spectrum (all bins equal magnitude)
    float expected_mag = 1.0f;
    for (int i = 0; i < 51; ++i) {
      REQUIRE_THAT(std::abs(spectrum[i]), WithinAbs(expected_mag, 1e-5f));
    }
  }

  SECTION("n_fft = 48") {
    FFT fft(48);
    REQUIRE(fft.n_fft() == 48);
    REQUIRE(fft.n_bins() == 25);

    // Roundtrip test
    std::vector<float> input(48);
    for (int i = 0; i < 48; ++i) {
      input[i] = std::sin(kTwoPi * 3 * i / 48);
    }

    std::vector<std::complex<float>> spectrum(25);
    fft.forward(input.data(), spectrum.data());

    std::vector<float> output(48);
    fft.inverse(spectrum.data(), output.data());

    for (int i = 0; i < 48; ++i) {
      REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-5f));
    }
  }
}

TEST_CASE("FFT phase accuracy", "[fft]") {
  constexpr int n_fft = 64;
  FFT fft(n_fft);

  SECTION("cosine wave has zero phase at peak bin") {
    // Cosine starts at maximum, so phase at peak bin should be 0
    std::vector<float> input(n_fft);
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::cos(kTwoPi * 8 * i / n_fft);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin8 = std::arg(spectrum[8]);
    REQUIRE_THAT(phase_at_bin8, WithinAbs(0.0f, 1e-5f));
  }

  SECTION("sine wave has -pi/2 phase at peak bin") {
    // Sine starts at zero going up, so phase at peak bin should be -pi/2
    std::vector<float> input(n_fft);
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::sin(kTwoPi * 8 * i / n_fft);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin8 = std::arg(spectrum[8]);
    REQUIRE_THAT(phase_at_bin8, WithinAbs(-kPi / 2.0f, 1e-5f));
  }

  SECTION("shifted cosine has correct phase") {
    // Cosine with quarter-wave shift should have phase = -pi/2
    std::vector<float> input(n_fft);
    float shift = kPi / 2.0f;  // Quarter period shift
    for (int i = 0; i < n_fft; ++i) {
      input[i] = std::cos(kTwoPi * 4 * i / n_fft - shift);
    }

    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(input.data(), spectrum.data());

    float phase_at_bin4 = std::arg(spectrum[4]);
    REQUIRE_THAT(phase_at_bin4, WithinAbs(-shift, 1e-4f));
  }
}

TEST_CASE("FFT energy preservation (Parseval's theorem)", "[fft]") {
  constexpr int n_fft = 128;
  FFT fft(n_fft);

  // Random-ish signal
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = std::sin(kTwoPi * 3 * i / n_fft) + 0.5f * std::cos(kTwoPi * 7 * i / n_fft) +
               0.3f * std::sin(kTwoPi * 15 * i / n_fft);
  }

  // Time domain energy
  float time_energy = 0.0f;
  for (int i = 0; i < n_fft; ++i) {
    time_energy += input[i] * input[i];
  }

  // Frequency domain energy
  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  float freq_energy = 0.0f;
  // DC and Nyquist bins count once, others count twice (due to symmetry)
  freq_energy += std::norm(spectrum[0]);                  // DC
  freq_energy += std::norm(spectrum[fft.n_bins() - 1]);   // Nyquist
  for (int i = 1; i < fft.n_bins() - 1; ++i) {
    freq_energy += 2.0f * std::norm(spectrum[i]);
  }
  freq_energy /= static_cast<float>(n_fft);

  REQUIRE_THAT(freq_energy, WithinAbs(time_energy, 1e-4f));
}

TEST_CASE("FFT multiple frequencies detection", "[fft]") {
  constexpr int n_fft = 256;
  FFT fft(n_fft);

  // Composite signal with frequencies at bins 10, 25, and 50
  std::vector<float> input(n_fft);
  for (int i = 0; i < n_fft; ++i) {
    input[i] = 1.0f * std::sin(kTwoPi * 10 * i / n_fft) +
               0.5f * std::sin(kTwoPi * 25 * i / n_fft) +
               0.25f * std::sin(kTwoPi * 50 * i / n_fft);
  }

  std::vector<std::complex<float>> spectrum(fft.n_bins());
  fft.forward(input.data(), spectrum.data());

  // Check magnitudes at expected bins (sine has amplitude/2 * n_fft)
  float mag10 = std::abs(spectrum[10]);
  float mag25 = std::abs(spectrum[25]);
  float mag50 = std::abs(spectrum[50]);

  // Relative magnitudes should be 1.0 : 0.5 : 0.25
  REQUIRE_THAT(mag25 / mag10, WithinAbs(0.5f, 0.01f));
  REQUIRE_THAT(mag50 / mag10, WithinAbs(0.25f, 0.01f));

  // Other bins should be near zero
  for (int i = 0; i < fft.n_bins(); ++i) {
    if (i != 10 && i != 25 && i != 50) {
      REQUIRE(std::abs(spectrum[i]) < mag50 * 0.1f);
    }
  }
}
