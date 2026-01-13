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
