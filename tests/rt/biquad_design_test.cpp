#include "rt/biquad_design.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <iterator>
#include <limits>

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
  // The helper is also safe when an unchecked realtime caller supplies a
  // sample rate below the 20 Hz frequency floor. Its clamp bounds must remain
  // ordered rather than invoking std::clamp with lo > hi.
  const float low_rate_w0 = sonare::rt::frequency_to_w0(1000.0f, 1.0);
  REQUIRE(std::isfinite(low_rate_w0));
  REQUIRE_THAT(low_rate_w0, WithinAbs(20.0f * sonare::constants::kTwoPi, 1.0e-6f));
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

TEST_CASE("rbj_peak falls back to passthrough when the gain overflows its taps", "[rt][biquad]") {
  // 10^(gain_db/40) overflows past roughly 12330 dB, leaving b0 = +inf and
  // b2 = -inf while a0 stays 1.0 — the shape normalize() used to let through
  // because it inspected a0 alone.
  const float w0 = static_cast<float>(2.0 * sonare::constants::kPiD * 1000.0 / 48000.0);
  const auto sane = sonare::rt::rbj_peak(w0, 1.0f, 6.0f);
  REQUIRE(std::isfinite(sane.b0));
  REQUIRE(sane.b0 != 1.0f);

  for (const float gain_db :
       {20000.0f, 1.0e30f, -1.0e30f, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    const auto coeffs = sonare::rt::rbj_peak(w0, 1.0f, gain_db);
    INFO(gain_db);
    REQUIRE(std::isfinite(coeffs.b0));
    REQUIRE(std::isfinite(coeffs.b1));
    REQUIRE(std::isfinite(coeffs.b2));
    REQUIRE(std::isfinite(coeffs.a1));
    REQUIRE(std::isfinite(coeffs.a2));
  }
}

TEST_CASE("first-order shelves put half their gain on the corner", "[rt][biquad]") {
  constexpr double kRate = 48000.0;
  const auto db = [](const sonare::rt::BiquadCoeffs& c, double hz) {
    const auto w = static_cast<float>(2.0 * sonare::constants::kPiD * hz / kRate);
    return 20.0 * std::log10(static_cast<double>(sonare::rt::biquad_magnitude(c, w)));
  };
  for (const double corner : {161.0, 1000.0, 6987.0}) {
    const auto w0 = static_cast<float>(2.0 * sonare::constants::kPiD * corner / kRate);
    for (const float gain_db : {12.0f, -12.0f, 3.0f}) {
      const auto low = sonare::rt::first_order_low_shelf(w0, gain_db);
      const auto high = sonare::rt::first_order_high_shelf(w0, gain_db);
      INFO("corner " << corner << " Hz, gain " << gain_db << " dB");
      REQUIRE(low.b2 == 0.0f);
      REQUIRE(low.a2 == 0.0f);
      REQUIRE(high.b2 == 0.0f);
      REQUIRE(high.a2 == 0.0f);
      REQUIRE_THAT(db(low, corner), WithinAbs(gain_db / 2.0, 1.0e-2));
      REQUIRE_THAT(db(high, corner), WithinAbs(gain_db / 2.0, 1.0e-2));
      REQUIRE_THAT(db(low, 0.0), WithinAbs(gain_db, 1.0e-3));
      REQUIRE_THAT(db(low, kRate / 2.0), WithinAbs(0.0, 1.0e-3));
      REQUIRE_THAT(db(high, 0.0), WithinAbs(0.0, 1.0e-3));
      REQUIRE_THAT(db(high, kRate / 2.0), WithinAbs(gain_db, 1.0e-3));
    }
    // A cut is its boost turned over, so the two cancel at every frequency.
    const auto boost = sonare::rt::first_order_low_shelf(w0, 12.0f);
    const auto cut = sonare::rt::first_order_low_shelf(w0, -12.0f);
    for (const double hz : {corner / 4.0, corner, corner * 4.0}) {
      REQUIRE_THAT(db(boost, hz) + db(cut, hz), WithinAbs(0.0, 1.0e-2));
    }
  }
}

namespace {

bool strictly_stable(const sonare::rt::BiquadCoeffs& c) {
  const double a1 = c.a1;
  const double a2 = c.a2;
  return std::abs(a2) < 1.0 && std::abs(a1) < 1.0 + a2;
}

bool all_finite(const sonare::rt::BiquadCoeffs& c) {
  return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) && std::isfinite(c.a1) &&
         std::isfinite(c.a2);
}

}  // namespace

TEST_CASE("every float biquad designer returns finite taps stable as stored", "[rt][biquad]") {
  namespace rt = sonare::rt;
  using sonare::constants::kPi;
  constexpr float kInf = std::numeric_limits<float>::infinity();
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  // From below the domain to past Nyquist, the float neighbours of pi included.
  const float angles[] = {-1.0f,
                          0.0f,
                          1.0e-9f,
                          1.0e-7f,
                          1.0e-6f,
                          1.0e-5f,
                          1.0e-4f,
                          3.0e-4f,
                          1.0e-3f,
                          0.01f,
                          0.1f,
                          1.0f,
                          3.0f,
                          kPi - 1.0e-3f,
                          kPi - 1.0e-6f,
                          kPi,
                          std::nextafter(kPi, 4.0f),
                          10.0f,
                          kInf,
                          kNaN};
  const float qs[] = {-1.0f, 0.0f,  1.0e-9f, 1.0e-6f, 1.0e-3f, 0.01f, 0.5f, 0.7071f,
                      1.0f,  10.0f, 1.0e3f,  1.0e5f,  1.0e7f,  kInf,  kNaN};
  const float gains[] = {-60.0f, -24.0f, -6.0f, 0.0f, 6.0f, 24.0f, 60.0f};
  for (const float w0 : angles) {
    for (const float q : qs) {
      for (const float gain : gains) {
        const rt::BiquadCoeffs designs[] = {rt::rbj_lowpass(w0, q),
                                            rt::rbj_highpass(w0, q),
                                            rt::rbj_bandpass(w0, q),
                                            rt::rbj_notch(w0, q),
                                            rt::rbj_allpass(w0, q),
                                            rt::rbj_peak(w0, q, gain),
                                            rt::rbj_low_shelf(w0, q, gain),
                                            rt::rbj_high_shelf(w0, q, gain),
                                            rt::vicanek_lowpass(w0, q),
                                            rt::vicanek_highpass(w0, q),
                                            rt::vicanek_bandpass(w0, q),
                                            rt::vicanek_notch(w0, q),
                                            rt::vicanek_peak(w0, q, gain),
                                            rt::vicanek_high_shelf(w0, gain),
                                            rt::vicanek_low_shelf(w0, gain),
                                            rt::first_order_lowpass(w0),
                                            rt::first_order_highpass(w0),
                                            rt::first_order_low_shelf(w0, gain),
                                            rt::first_order_high_shelf(w0, gain)};
        for (size_t d = 0; d < std::size(designs); ++d) {
          INFO("designer " << d << ", w0 " << w0 << ", q " << q << ", gain " << gain);
          REQUIRE(all_finite(designs[d]));
          REQUIRE(strictly_stable(designs[d]));
        }
        // The cut filters keep their passband at unity as stored, however close
        // the rounded denominator sits to the unit circle.
        const auto low = rt::rbj_lowpass(w0, q);
        const auto high = rt::rbj_highpass(w0, q);
        INFO("w0 " << w0 << ", q " << q);
        REQUIRE_THAT((static_cast<double>(low.b0) + low.b1 + low.b2) / (1.0 + low.a1 + low.a2),
                     WithinAbs(1.0, 1.0e-6));
        REQUIRE_THAT((static_cast<double>(high.b0) - high.b1 + high.b2) / (1.0 - high.a1 + high.a2),
                     WithinAbs(1.0, 1.0e-6));
      }
    }
  }
}

TEST_CASE("an overdamped Vicanek section stays finite at a low Q", "[rt][biquad]") {
  const auto w0 = static_cast<float>(2.0 * sonare::constants::kPiD * 16000.0 / 48000.0);
  sonare::rt::BiquadState state;
  state.set(sonare::rt::vicanek_lowpass(w0, 0.01f));
  REQUIRE(all_finite(state.c));
  int non_finite = 0;
  for (int i = 0; i < 512; ++i) {
    if (!std::isfinite(state.process(i == 0 ? 1.0f : 0.0f))) ++non_finite;
  }
  REQUIRE(non_finite == 0);
}

TEST_CASE("the design ceiling sits strictly below Nyquist in float", "[rt][biquad]") {
  for (const double rate : {1000.0, 8000.0, 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0,
                            384000.0, 768000.0}) {
    const float ceiling = sonare::rt::max_design_frequency_hz(rate);
    INFO("rate " << rate);
    REQUIRE(ceiling > 0.0f);
    REQUIRE(ceiling < static_cast<float>(rate * 0.5));
  }
  REQUIRE(sonare::rt::max_design_frequency_hz(0.0) == 0.0f);
  const auto domain = sonare::rt::clamp_design_domain(std::numeric_limits<double>::quiet_NaN(),
                                                      std::numeric_limits<double>::infinity());
  REQUIRE(domain.w0 == sonare::rt::kMinDesignW0);
  REQUIRE(domain.q == sonare::rt::kMaxDesignQ);
  REQUIRE(sonare::rt::clamp_design_domain(4.0, 1.0).w0 == sonare::rt::kMaxDesignW0);
}
