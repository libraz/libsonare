/// @file adaa2_test.cpp
/// @brief Tests for second-order antiderivative antialiasing (Adaa2).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "rt/adaa.h"
#include "rt/aliasing_control.h"
#include "rt/nonlinearities.h"
#include "util/constants.h"

using sonare::FFT;
using sonare::constants::kPi;
using sonare::constants::kTwoPi;
using sonare::rt::Adaa1;
using sonare::rt::Adaa2;
using sonare::rt::ArctanNonlinearity;
using sonare::rt::CubicSoftClipNonlinearity;
using sonare::rt::HardClipNonlinearity;

TEST_CASE("Adaa2 converges to the nonlinearity for constant input", "[adaa][adaa2]") {
  constexpr int kNumSamples = 1000;
  constexpr int kWarmup = 3;

  SECTION("HardClip") {
    HardClipNonlinearity nl{};
    nl.limit = 1.0f;
    for (float c : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
      Adaa2<HardClipNonlinearity> adaa(nl);
      const float expected = std::clamp(c, -1.0f, 1.0f);
      for (int n = 0; n < kNumSamples; ++n) {
        const float y = adaa.process(c);
        if (n >= kWarmup) {
          CAPTURE(c, n, y, expected);
          REQUIRE(std::abs(y - expected) < 1.0e-4f);
        }
      }
    }
  }

  SECTION("Arctan") {
    ArctanNonlinearity nl{};
    for (float c : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
      Adaa2<ArctanNonlinearity> adaa(nl);
      const float expected = nl.apply(c);
      for (int n = 0; n < kNumSamples; ++n) {
        const float y = adaa.process(c);
        if (n >= kWarmup) {
          CAPTURE(c, n, y, expected);
          REQUIRE(std::abs(y - expected) < 1.0e-4f);
        }
      }
    }
  }

  SECTION("CubicSoftClip") {
    CubicSoftClipNonlinearity nl{};
    for (float c : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
      Adaa2<CubicSoftClipNonlinearity> adaa(nl);
      const float expected = nl.apply(c);
      for (int n = 0; n < kNumSamples; ++n) {
        const float y = adaa.process(c);
        if (n >= kWarmup) {
          CAPTURE(c, n, y, expected);
          REQUIRE(std::abs(y - expected) < 1.0e-4f);
        }
      }
    }
  }
}

TEST_CASE("Adaa2 reduces aliasing energy below Adaa1 and direct clipping", "[adaa][adaa2]") {
  constexpr int kN = 65536;
  constexpr float kSr = 22050.0f;
  constexpr float kFreq = 7717.0f;

  // Generate an over-driven sine; hard clipping injects strong harmonics that
  // fold back as aliases at this sample rate.
  std::vector<float> input(kN);
  for (int n = 0; n < kN; ++n) {
    input[n] = 2.0f * std::sin(kTwoPi * kFreq * static_cast<float>(n) / kSr);
  }

  HardClipNonlinearity nl{};
  nl.limit = 1.0f;

  std::vector<float> direct(kN);
  std::vector<float> a1(kN);
  std::vector<float> a2(kN);

  Adaa1<HardClipNonlinearity> adaa1(nl);
  Adaa2<HardClipNonlinearity> adaa2(nl);
  for (int n = 0; n < kN; ++n) {
    direct[n] = std::clamp(input[n], -1.0f, 1.0f);
    a1[n] = adaa1.process(input[n]);
    a2[n] = adaa2.process(input[n]);
  }

  // Sum spectral magnitude in bins that are NOT close to harmonics of the
  // fundamental (including their Nyquist-folded images).
  const auto alias_energy = [&](const std::vector<float>& sig) {
    FFT fft(kN);
    std::vector<float> windowed(kN);
    for (int n = 0; n < kN; ++n) {
      const float w = 0.5f * (1.0f - std::cos(kTwoPi * static_cast<float>(n) / (kN - 1)));
      windowed[n] = sig[n] * w;
    }
    std::vector<std::complex<float>> spectrum(fft.n_bins());
    fft.forward(windowed.data(), spectrum.data());

    const float bin_hz = kSr / static_cast<float>(kN);
    const float nyquist = kSr * 0.5f;
    // Tolerance around harmonic bins, in bins.
    constexpr int kGuard = 8;

    // Precompute folded harmonic frequencies up to Nyquist.
    std::vector<float> harmonics;
    for (int k = 1; k < 64; ++k) {
      float f = kFreq * static_cast<float>(k);
      // Fold into [0, nyquist].
      f = std::fmod(f, kSr);
      if (f > nyquist) f = kSr - f;
      harmonics.push_back(f);
    }

    double energy = 0.0;
    for (int b = 1; b < fft.n_bins(); ++b) {
      const float freq = static_cast<float>(b) * bin_hz;
      bool is_harmonic = false;
      for (float h : harmonics) {
        if (std::abs(freq - h) <= kGuard * bin_hz) {
          is_harmonic = true;
          break;
        }
      }
      if (!is_harmonic) {
        energy += std::abs(spectrum[b]);
      }
    }
    return energy;
  };

  const double e_none = alias_energy(direct);
  const double e_a1 = alias_energy(a1);
  const double e_a2 = alias_energy(a2);
  CAPTURE(e_none, e_a1, e_a2);

  // Each stage should reduce aliasing energy with a clear margin.
  REQUIRE(e_a1 < e_none * 0.9);
  REQUIRE(e_a2 < e_a1 * 0.9);
}

TEST_CASE("Adaa2 stays finite and bounded for extreme inputs", "[adaa][adaa2]") {
  HardClipNonlinearity nl{};
  nl.limit = 1.0f;
  // ADAA band-limits the output, so transient overshoot beyond the clip
  // ceiling (Gibbs-like ringing) is expected; only require a loose bound that
  // still catches genuine instability or blow-up.
  constexpr float kTol = 0.5f;

  SECTION("Nyquist-rate alternating input") {
    Adaa2<HardClipNonlinearity> adaa(nl);
    for (int n = 0; n < 4096; ++n) {
      const float x = (n % 2 == 0) ? 2.0f : -2.0f;
      const float y = adaa.process(x);
      CAPTURE(n, x, y);
      REQUIRE(std::isfinite(y));
      REQUIRE(std::abs(y) <= nl.limit + kTol);
    }
  }

  SECTION("Noisy ramp input") {
    Adaa2<HardClipNonlinearity> adaa(nl);
    float acc = 0.0f;
    for (int n = 0; n < 8192; ++n) {
      // Pseudo-random-ish ramp spanning beyond the clip region.
      acc += 0.013f;
      const float x = 3.0f * std::sin(acc) + 0.5f * std::sin(17.0f * acc);
      const float y = adaa.process(x);
      CAPTURE(n, x, y);
      REQUIRE(std::isfinite(y));
      REQUIRE(std::abs(y) <= nl.limit + kTol);
    }
  }
}
