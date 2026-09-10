/// @file nonlinearities_test.cpp
/// @brief Accuracy of the ADAA antiderivatives at small arguments.
///
/// An antiderivative only has to be accurate where its own divided difference
/// is taken, and ADAA takes that difference at every amplitude a signal passes
/// through on its way down. A closed form that cancels away its significant
/// bits near zero therefore does not merely lose precision: the error survives
/// the division by a sample step and becomes broadband noise that outlives the
/// signal it came from. These cases pin the property that prevents it.

#include "rt/nonlinearities.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "rt/adaa.h"
#include "util/constants.h"

using sonare::FFT;
using sonare::constants::kTwoPi;
using sonare::rt::Adaa1;
using sonare::rt::PushPullNonlinearity;
using sonare::rt::TanhNonlinearity;

TEST_CASE("log(cosh) keeps its significant bits near zero", "[adaa][nonlinearities]") {
  const TanhNonlinearity nl;

  SECTION("it is strictly positive away from the origin") {
    // log(cosh x) is zero only at zero, so a sign is a whole-value error rather
    // than a rounding one. The step matters: on a 1e-4 grid the closed form
    // this replaced returns two exact zeros and no negative at all, and a grid
    // that coarse would have let it through.
    REQUIRE(nl.antiderivative(0.0f) == 0.0f);
    for (int i = 1; i <= 100000; ++i) {
      const float x = static_cast<float>(i) * 1.0e-6f;
      CAPTURE(x);
      REQUIRE(nl.antiderivative(x) > 0.0f);
      REQUIRE(nl.antiderivative(-x) == nl.antiderivative(x));
    }
    for (int i = 1; i <= 80000; ++i) {
      const float x = static_cast<float>(i) * 1.0e-4f;
      CAPTURE(x);
      REQUIRE(nl.antiderivative(x) > 0.0f);
    }
  }

  SECTION("it agrees with x^2/2 where that is the leading term") {
    // Below 1e-2 the quartic term is under 1e-4 of the total, so x^2/2 is the
    // answer to well inside float precision and any real error shows up here.
    for (float x : {1.0e-2f, 3.0e-3f, 1.0e-3f, 3.0e-4f, 1.0e-4f, 3.0e-5f, 1.0e-5f}) {
      const float expected = 0.5f * x * x;
      CAPTURE(x, nl.antiderivative(x), expected);
      REQUIRE(std::abs(nl.antiderivative(x) - expected) <= 1.0e-3f * expected);
    }
  }

  SECTION("a push-pull pair at rest is the same arithmetic") {
    // amp_sim relies on this collapse; a branch added for accuracy must not
    // break it, and a series that only one of the two paths takes would.
    const PushPullNonlinearity pair;
    for (int i = -80000; i <= 80000; ++i) {
      const float x = static_cast<float>(i) * 1.0e-4f;
      CAPTURE(x);
      REQUIRE(pair.antiderivative(x) == nl.antiderivative(x));
    }
  }
}

TEST_CASE("Adaa1 leaves no floor behind a decaying tone", "[adaa][nonlinearities]") {
  // The failure this guards against is audible rather than numeric: a note
  // decays into a band it never excited, and the band stops decaying with it.
  constexpr double kSampleRate = 96000.0;
  constexpr double kF0 = 220.0;
  constexpr double kDecayDbPerSecond = 12.0;
  constexpr int kNumSamples = 4 * static_cast<int>(kSampleRate);

  Adaa1<PushPullNonlinearity> adaa;
  std::vector<float> y(static_cast<size_t>(kNumSamples));
  for (int n = 0; n < kNumSamples; ++n) {
    const double t = n / kSampleRate;
    const double env = std::pow(10.0, -kDecayDbPerSecond * t / 20.0);
    // Driven well past the knee so the early part is genuine saturation.
    y[static_cast<size_t>(n)] = static_cast<float>(20.0 * env * std::sin(kTwoPi * kF0 * t));
  }
  for (float& s : y) s = adaa.process(s);

  // The last window, 48 dB down. The bound is placed by measurement rather than
  // by taste: the closed form this replaced reaches -55 dB here and the series
  // reaches -99, so anything between them discriminates and -75 leaves either
  // side about 20 dB. A bound outside that pair would pass both and assert
  // nothing, which is how this case first went in.
  constexpr int kWindow = 16384;
  std::vector<float> tail(y.end() - kWindow, y.end());
  for (int n = 0; n < kWindow; ++n) {
    tail[static_cast<size_t>(n)] *=
        static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * n / (kWindow - 1)));
  }
  FFT fft(kWindow);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(kWindow) / 2 + 1);
  fft.forward(tail.data(), spectrum.data());

  const double bin_hz = kSampleRate / kWindow;
  double fundamental = 0.0;
  double above = 0.0;
  for (size_t k = 0; k < spectrum.size(); ++k) {
    const double hz = static_cast<double>(k) * bin_hz;
    const double power = std::norm(spectrum[k]);
    if (hz > kF0 - 40.0 && hz < kF0 + 40.0) fundamental += power;
    // Between the harmonics the pair can produce and Nyquist there is nothing
    // a push-pull stage puts here from a single tone.
    if (hz > 6000.0 && hz < 20000.0) above += power;
  }
  const double ratio_db = 10.0 * std::log10(above / fundamental);
  CAPTURE(ratio_db);
  REQUIRE(ratio_db < -75.0);
}
