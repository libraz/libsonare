/// @file open_end_radiation_test.cpp
/// @brief The open-end termination (rt/open_end_radiation): one boundary whose
///        radiated output is the pressure the reflection leaves at the opening, a reflection that
///        never exceeds the termination's loss budget, and a cutoff that is the same in hertz at
///        every rate.

#include "rt/open_end_radiation.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>

#include "util/constants.h"

namespace {

using sonare::rt::OpenEndRadiation;

/// Steady-state peak of the reflected wave for a unit sine at @p hz.
double reflected_gain(double sr, float cutoff_hz, float loss, double hz) {
  OpenEndRadiation end;
  end.configure(sr, cutoff_hz, loss);
  const int settle = static_cast<int>(sr * 0.2);
  const int measure = static_cast<int>(sr * 0.1);
  double peak = 0.0;
  for (int n = 0; n < settle + measure; ++n) {
    const double phase = sonare::constants::kTwoPiD * hz * n / sr;
    const auto out = end.process(static_cast<float>(std::sin(phase)));
    if (n >= settle) peak = std::max(peak, std::fabs(static_cast<double>(out.reflected)));
  }
  return peak;
}

}  // namespace

TEST_CASE("open-end radiation returns both halves of one boundary", "[rt][open_end_radiation]") {
  OpenEndRadiation end;
  end.configure(48000.0, 1000.0f, 0.25f);
  OpenEndRadiation::Output out{};
  for (int n = 0; n < 48000; ++n) {
    out = end.process(1.0f);
    // Pressure at the opening is incoming plus reflected, and that is what radiates.
    REQUIRE(std::fabs(out.radiated - out.reflected - 1.0f) < 1.0e-6f);
  }
  // DC is a pressure release: inverted reflection less the loss, the loss radiates.
  REQUIRE(std::fabs(out.reflected + 0.75f) < 1.0e-5f);
  REQUIRE(std::fabs(out.radiated - 0.25f) < 1.0e-5f);
}

TEST_CASE("open-end radiation reflects no more than its loss allows at any frequency",
          "[rt][open_end_radiation]") {
  for (const float loss : {0.0f, 0.1f}) {
    for (const double hz : {50.0, 400.0, 1000.0, 4000.0, 15000.0, 23000.0}) {
      const double gain = reflected_gain(48000.0, 1000.0f, loss, hz);
      INFO("loss " << loss << " hz " << hz << " gain " << gain);
      REQUIRE(gain <= (1.0 - loss) * (1.0 + 1.0e-4));
    }
  }
  // Short wavelengths leave the tube rather than returning into it.
  REQUIRE(reflected_gain(48000.0, 1000.0f, 0.0f, 15000.0) < 0.1);
}

TEST_CASE("open-end radiation holds its cutoff in hertz across sample rates",
          "[rt][open_end_radiation]") {
  // At the corner a one-pole far below Nyquist reflects 1/sqrt(2) of the wave.
  const double asked = sonare::constants::kInvSqrt2D;
  const double at_48k = reflected_gain(48000.0, 1000.0f, 0.0f, 1000.0);
  const double at_44k = reflected_gain(44100.0, 1000.0f, 0.0f, 1000.0);
  INFO("48 kHz " << at_48k << " 44.1 kHz " << at_44k);
  REQUIRE(std::fabs(at_48k / asked - 1.0) < 0.02);
  REQUIRE(std::fabs(at_44k / asked - 1.0) < 0.02);
  REQUIRE(std::fabs(at_48k / at_44k - 1.0) < 0.005);
}

TEST_CASE("open-end radiation never returns more than arrived, counting its stored energy",
          "[rt][open_end_radiation]") {
  // The storage (1 - alpha) / alpha * state^2 is what lets a coupled system's energy include the
  // boundary.
  for (const float cutoff : {200.0f, 1000.0f, 4000.0f, 16000.0f}) {
    OpenEndRadiation end;
    end.configure(48000.0, cutoff, 0.0f);
    std::uint32_t state = 12345u;
    double worst = 0.0;
    for (int n = 0; n < 48000; ++n) {
      state = state * 1664525u + 1013904223u;
      const float incoming = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
      const double before = end.stored_energy();
      const auto out = end.process(incoming);
      const double balance = static_cast<double>(incoming) * incoming -
                             static_cast<double>(out.reflected) * out.reflected -
                             (end.stored_energy() - before);
      worst = std::min(worst, balance);
    }
    INFO("cutoff " << cutoff << " worst balance " << worst);
    REQUIRE(worst > -1.0e-5);
  }
}
