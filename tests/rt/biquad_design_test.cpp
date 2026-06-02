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
  constexpr double q = sonare::constants::kButterworthQD;

  const auto design = sonare::rt::rbj_high_shelf_design_d(frequency, sample_rate, q);
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, 0.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, 0.0, q));
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, 6.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, 6.0, q));
  require_close(sonare::rt::rbj_high_shelf_from_design_d(design, -3.0),
                sonare::rt::rbj_high_shelf_d(frequency, sample_rate, -3.0, q));
}

TEST_CASE("Vicanek high-shelf keeps DC/passband at unity across common settings", "[rt][biquad]") {
  // Regression: the high-shelf b2 numerator carried an extra 1/a0 factor, which
  // corrupted the passband (DC) gain. The validation guard only samples the HF
  // endpoint, so for cutoffs >= ~5 kHz the bad coefficients shipped silently.
  constexpr double sample_rate = 48000.0;
  const auto db = [](float linear) { return 20.0f * std::log10(std::max(linear, 1.0e-12f)); };
  for (double fc : {3000.0, 5000.0, 8000.0, 12000.0, 15000.0}) {
    const float w0 = static_cast<float>(sonare::constants::kTwoPiD * fc / sample_rate);
    for (float gain_db : {-6.0f, -3.0f, -1.0f, 1.0f, 3.0f, 6.0f}) {
      const auto coeffs = sonare::rt::vicanek_high_shelf(w0, gain_db);
      // DC (omega = 0) must stay at unity (0 dB) for a high shelf.
      REQUIRE_THAT(db(sonare::rt::biquad_magnitude(coeffs, 0.0f)), WithinAbs(0.0f, 0.75f));
      // High-frequency endpoint must approach the requested shelf gain.
      const float nyquist = static_cast<float>(sonare::constants::kPi * 0.999);
      REQUIRE_THAT(db(sonare::rt::biquad_magnitude(coeffs, nyquist)), WithinAbs(gain_db, 1.5f));
    }
  }
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

TEST_CASE("One-pole alpha from time_ms matches the time-domain form", "[rt][biquad]") {
  // Cross-check: the time-ms parameterization must match the legacy
  // voice_changer::coeff_ms formula `1 - exp(-1 / (tau * sample_rate))` for
  // sane inputs, and saturate cleanly for sub-floor and degenerate inputs.
  constexpr double sample_rate = 48000.0;
  for (float ms : {0.1f, 1.0f, 5.0f, 50.0f, 500.0f}) {
    const float tau_sec = ms * 0.001f;
    const double expected = 1.0 - std::exp(-1.0 / (static_cast<double>(tau_sec) * sample_rate));
    REQUIRE_THAT(sonare::rt::one_pole_alpha_from_time_ms(ms, sample_rate),
                 WithinAbs(static_cast<float>(expected), 1.0e-6f));
  }
  // Floor of 0.05 ms — anything smaller (including 0 and negatives) collapses
  // to the floor value so the result stays in [0, 1] and avoids divide-by-zero.
  REQUIRE(sonare::rt::one_pole_alpha_from_time_ms(0.0f, sample_rate) ==
          sonare::rt::one_pole_alpha_from_time_ms(0.05f, sample_rate));
  REQUIRE(sonare::rt::one_pole_alpha_from_time_ms(-100.0f, sample_rate) ==
          sonare::rt::one_pole_alpha_from_time_ms(0.05f, sample_rate));
  // Non-positive sample rate is degenerate; report 1.0 (fully open) like the
  // matched/frequency form.
  REQUIRE(sonare::rt::one_pole_alpha_from_time_ms(10.0f, -1.0) == 1.0f);
}

TEST_CASE("frequency_to_w0 clamps frequency to a safe range", "[rt][biquad]") {
  constexpr double sample_rate = 48000.0;
  // Mid-band frequency: linear map 2*pi*f/sr.
  const float expected_mid = static_cast<float>(sonare::constants::kTwoPiD * 1000.0 / sample_rate);
  REQUIRE_THAT(sonare::rt::frequency_to_w0(1000.0f, sample_rate), WithinAbs(expected_mid, 1.0e-7f));
  // Sub-20 Hz must clamp UP to 20 Hz.
  REQUIRE(sonare::rt::frequency_to_w0(1.0f, sample_rate) ==
          sonare::rt::frequency_to_w0(20.0f, sample_rate));
  // Above 0.45 * Nyquist must clamp DOWN; biquad designs become unstable as
  // w0 approaches pi, so callers rely on this safety net.
  const float nyq_safe = static_cast<float>(sample_rate * 0.45);
  REQUIRE(sonare::rt::frequency_to_w0(static_cast<float>(sample_rate), sample_rate) ==
          sonare::rt::frequency_to_w0(nyq_safe, sample_rate));
  // Non-positive sample rate is degenerate; report 0.
  REQUIRE(sonare::rt::frequency_to_w0(1000.0f, -1.0) == 0.0f);
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
