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

namespace {

/// Composite Simpson quadrature of @p f over [a, b], in double.
template <typename F>
double simpson(F f, double a, double b, int panels = 64) {
  const double h = (b - a) / (2.0 * panels);
  double sum = f(a) + f(b);
  for (int i = 1; i < 2 * panels; ++i) {
    sum += f(a + h * i) * ((i % 2) ? 4.0 : 2.0);
  }
  return sum * h / 3.0;
}

}  // namespace

TEST_CASE("Adaa1 hard clip never exceeds its ceiling", "[adaa][nonlinearities]") {
  for (float ceiling : {0.5f, 0.7f, 1.0f}) {
    for (float sign : {1.0f, -1.0f}) {
      for (float level : {1.5f, 2.3f, 4.0f, 7.7f, 16.0f}) {
        for (float step : {1.1e-5f, 2.0e-5f, 1.0e-4f, 1.0e-3f}) {
          Adaa1<sonare::rt::HardClipNonlinearity> adaa(sonare::rt::HardClipNonlinearity{ceiling});
          adaa.reset(sign * level);
          for (int n = 1; n <= 8; ++n) {
            const float y = adaa.process(sign * (level + step * static_cast<float>(n)));
            CAPTURE(ceiling, level, step, y);
            REQUIRE(std::abs(y) <= ceiling);
          }
        }
      }
    }
  }
}

TEST_CASE("Adaa1 tanh and arctan stay within the curve bound at large drive",
          "[adaa][nonlinearities]") {
  const double kHalfPi = std::acos(0.0);
  for (float level : {3.0f, 6.0f, 12.0f, 25.0f, 40.0f}) {
    for (float step : {1.1e-5f, 3.0e-5f, 3.0e-4f}) {
      Adaa1<TanhNonlinearity> tanh_adaa;
      Adaa1<sonare::rt::ArctanNonlinearity> atan_adaa;
      tanh_adaa.reset(level);
      atan_adaa.reset(level);
      for (int n = 1; n <= 8; ++n) {
        const float x = level + step * static_cast<float>(n);
        const float yt = tanh_adaa.process(x);
        const float ya = atan_adaa.process(x);
        CAPTURE(level, step, yt, ya);
        REQUIRE(std::abs(yt) <= 1.0f + 4.0f * 1.2e-7f);
        REQUIRE(std::abs(ya) <= static_cast<float>(kHalfPi) * (1.0f + 4.0f * 1.2e-7f));
      }
    }
  }
}

TEST_CASE("Adaa1 push-pull interval average has the transfer's sign and value",
          "[adaa][nonlinearities]") {
  const float bias = 1.3f;
  const float knee = 1.0f + 0.9f * bias * bias;
  const PushPullNonlinearity shape{bias, knee};

  // Positive interval of the odd transfer's positive branch, above the divisor guard.
  const float x0 = 0.29975f;
  const float x1 = 0.29975f + 4.0e-5f;
  Adaa1<PushPullNonlinearity> adaa(shape);
  adaa.reset(x0);
  const double y = adaa.process(x1);
  const double reference =
      simpson([&](double x) { return static_cast<double>(shape.apply(static_cast<float>(x))); }, x0,
              x1) /
      (static_cast<double>(x1) - static_cast<double>(x0));
  CAPTURE(y, reference);
  REQUIRE(reference > 0.0);
  REQUIRE(y > 0.0);
  REQUIRE(std::abs(y - reference) <= 1.0e-5 * std::abs(reference) + 1.0e-7);
}

TEST_CASE("Adaa1 re-evaluates its previous primitive when the shape changes",
          "[adaa][nonlinearities]") {
  // A step in the shape on a nearly flat input must not turn the change of the
  // antiderivative constant into an impulse of order 1/dx.
  Adaa1<PushPullNonlinearity> adaa(PushPullNonlinearity{0.0f, 1.0f});
  const float x = 0.6f;
  adaa.reset(x);
  for (int n = 0; n < 4; ++n) adaa.process(x + 3.0e-5f * static_cast<float>(n));

  const PushPullNonlinearity retargeted{1.3f, 1.0f + 0.9f * 1.3f * 1.3f};
  adaa.set_nonlinearity(retargeted);
  float last = x + 3.0e-5f * 3.0f;
  for (int n = 4; n < 12; ++n) {
    last += 3.0e-5f;
    const float y = adaa.process(last);
    CAPTURE(n, y);
    REQUIRE(std::abs(y) <= 1.0f);
    REQUIRE(std::abs(y - retargeted.apply(last)) <= 1.0e-3f);
  }
}
