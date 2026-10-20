#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "rt/biquad_design.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::rt;

TEST_CASE("one-pole frequency pole matches the closed form and complements the alpha",
          "[rt][one_pole]") {
  for (const double sr : {44100.0, 48000.0}) {
    for (const double fc : {5.0, 120.0, 4000.0}) {
      const double pole = one_pole_pole_matched_d(fc, sr);
      REQUIRE_THAT(pole, WithinAbs(std::exp(-sonare::constants::kTwoPiD * fc / sr), 1e-15));
      REQUIRE_THAT(pole + one_pole_lowpass_alpha_matched_d(fc, sr), WithinAbs(1.0, 1e-15));
    }
  }
  REQUIRE(one_pole_pole_matched_d(0.0, 48000.0) == 1.0);
  REQUIRE(one_pole_pole_matched_d(100.0, 0.0) == 0.0);
}

TEST_CASE("one-pole time-constant helpers match the closed form and each other", "[rt][one_pole]") {
  for (const double sr : {44100.0, 48000.0}) {
    for (const double tau : {1e-5, 0.005, 0.04, 0.5}) {
      const double pole = one_pole_pole_from_seconds_d(tau, sr);
      const double alpha = one_pole_alpha_from_seconds_d(tau, sr);
      REQUIRE_THAT(pole, WithinAbs(std::exp(-1.0 / (tau * sr)), 1e-15));
      REQUIRE_THAT(pole + alpha, WithinAbs(1.0, 1e-15));
      // The impulse response of y += alpha * (x - y) reaches 1/e after tau seconds.
      REQUIRE_THAT(std::pow(pole, tau * sr), WithinAbs(std::exp(-1.0), 1e-9));
    }
  }
}

TEST_CASE("one-pole time-constant helpers agree with the millisecond helper above its floor",
          "[rt][one_pole]") {
  for (const double sr : {44100.0, 48000.0}) {
    for (const float ms : {0.1f, 5.0f, 40.0f, 300.0f}) {
      REQUIRE_THAT(one_pole_alpha_from_seconds_d(ms * 0.001, sr),
                   WithinAbs(one_pole_alpha_from_time_ms(ms, sr), 1e-5));
    }
  }
}

TEST_CASE("one-pole time-constant helpers saturate on non-positive input", "[rt][one_pole]") {
  REQUIRE(one_pole_alpha_from_seconds_d(0.0, 48000.0) == 1.0);
  REQUIRE(one_pole_alpha_from_seconds_d(0.01, 0.0) == 1.0);
  REQUIRE(one_pole_pole_from_seconds_d(0.0, 48000.0) == 0.0);
  REQUIRE(one_pole_pole_from_seconds_d(0.01, 0.0) == 0.0);
}
