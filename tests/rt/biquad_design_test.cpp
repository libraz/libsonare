#include "rt/biquad_design.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {

void require_close(const sonare::rt::BiquadCoeffsD& actual,
                   const sonare::rt::BiquadCoeffsD& expected, double tolerance = 1.0e-12) {
  REQUIRE_THAT(actual.b0, WithinAbs(expected.b0, tolerance));
  REQUIRE_THAT(actual.b1, WithinAbs(expected.b1, tolerance));
  REQUIRE_THAT(actual.b2, WithinAbs(expected.b2, tolerance));
  REQUIRE_THAT(actual.a1, WithinAbs(expected.a1, tolerance));
  REQUIRE_THAT(actual.a2, WithinAbs(expected.a2, tolerance));
}

}  // namespace

TEST_CASE("RBJ high-shelf cached design matches direct double design", "[rt][biquad]") {
  constexpr double frequency = 10000.0;
  constexpr double sample_rate = 48000.0;
  constexpr double q = 1.0 / 1.4142135623730951;

  const auto design = sonare::rt::rbj_high_shelf_design_d(frequency, sample_rate, q);
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, 0.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, 0.0, q));
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, 6.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, 6.0, q));
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, -3.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, -3.0, q));
}

TEST_CASE("Butterworth stage Q helper matches expected cascade values", "[rt][biquad]") {
  REQUIRE_THAT(sonare::rt::butterworth_stage_q(2, 0),
               WithinAbs(sonare::constants::kButterworthQ, 1.0e-6f));
  REQUIRE_THAT(sonare::rt::butterworth_stage_q(4, 0), WithinAbs(1.306563f, 1.0e-6f));
  REQUIRE_THAT(sonare::rt::butterworth_stage_q(4, 1), WithinAbs(0.541196f, 1.0e-6f));
}

TEST_CASE("One-pole low-pass alpha helper matches legacy bilinear form", "[rt][biquad]") {
  constexpr float frequency = 180.0f;
  constexpr double sample_rate = 48000.0;
  const double g = 2.0 * sonare::constants::kPiD * frequency;
  REQUIRE_THAT(sonare::rt::one_pole_lowpass_alpha(frequency, sample_rate),
               WithinAbs(static_cast<float>(g / (g + sample_rate)), 1.0e-7f));
  REQUIRE(sonare::rt::one_pole_lowpass_alpha(-1.0f, sample_rate) == 0.0f);
  REQUIRE(sonare::rt::one_pole_lowpass_alpha(1000.0f, -1.0) == 1.0f);
}

TEST_CASE("Matched one-pole low-pass alpha helper matches legacy exponential form",
          "[rt][biquad]") {
  constexpr float frequency = 180.0f;
  constexpr double sample_rate = 48000.0;
  const double expected = 1.0 - std::exp(-2.0 * sonare::constants::kPiD * frequency / sample_rate);
  REQUIRE_THAT(sonare::rt::one_pole_lowpass_alpha_matched(frequency, sample_rate),
               WithinAbs(static_cast<float>(expected), 1.0e-7f));
  REQUIRE(sonare::rt::one_pole_lowpass_alpha_matched(-1.0f, sample_rate) == 0.0f);
  REQUIRE(sonare::rt::one_pole_lowpass_alpha_matched(1000.0f, -1.0) == 1.0f);
}

TEST_CASE("K-weighting uses BS.1770 DeMan design away from 48 kHz", "[rt][biquad]") {
  const auto at_48k = sonare::rt::k_weighting_coefficients(48000.0);
  require_close(
      at_48k.pre,
      {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585},
      1.0e-12);
  require_close(at_48k.rlb, {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621}, 1.0e-12);

  const auto at_22050 = sonare::rt::k_weighting_coefficients(22050.0);
  require_close(at_22050.pre,
                {1.479825350977812, -2.170728612856829, 0.860842484726486, -1.338305336066134,
                 0.508244558913602},
                1.0e-12);
  require_close(at_22050.rlb, {1.0, -2.0, 1.0, -1.978397602590054, 0.978514419503187}, 1.0e-12);
}
