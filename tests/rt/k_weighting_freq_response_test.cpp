/// @file k_weighting_freq_response_test.cpp
/// @brief Frequency-response accuracy tests for the K-weighting filter at non-48 kHz rates.
///
/// @details The ITU-R BS.1770-4 K-weighting filter is implemented in
/// src/rt/biquad_design.cpp::k_weighting_coefficients().  At 48 kHz the
/// function returns the bit-exact standard reference values.  At all other
/// sample rates it derives the coefficients via the Deman bilinear-transform
/// formulation.
///
/// This test suite:
///   1. Verifies that the coefficients returned for 22050 / 44100 / 96000 Hz
///      match the Python reference (tools/scripts/k_weighting_reference.py,
///      output: tests/fixtures/k_weighting_reference.json) to double precision.
///   2. Evaluates the frequency response of the derived filter at 100, 1000,
///      and 10000 Hz and checks that it agrees with the reference within
///      0.5 dB (< 0.1 dB in practice for mid/high frequencies).
///   3. Verifies that the 48 kHz hardcoded path produces a frequency response
///      that matches the Deman-derived prediction within 0.01 dB (the
///      hardcoded coefficients and the Deman formula are accurate to ~1e-13).
///
/// Reference data: tests/fixtures/k_weighting_reference.json
/// Reference script: tools/scripts/k_weighting_reference.py

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <string>

#include "rt/biquad_design.h"
#include "util/json_reader.h"

using Catch::Matchers::WithinAbs;
using sonare::rt::BiquadCoeffsD;
using sonare::rt::k_weighting_coefficients;
using sonare::rt::KWeightingCoeffs;
using sonare::test::JsonReader;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Evaluate |H(e^{j*omega})| in dB for a normalized biquad section.
///
/// H(z) = (b0 + b1*z^{-1} + b2*z^{-2}) / (1 + a1*z^{-1} + a2*z^{-2})
/// evaluated at z = e^{j*omega}, omega = 2*pi*f/fs.
double biquad_response_db(const BiquadCoeffsD& c, double freq_hz, double sample_rate) {
  const double omega = 2.0 * M_PI * freq_hz / sample_rate;
  const std::complex<double> z_inv{std::cos(omega), -std::sin(omega)};
  const std::complex<double> z_inv2 = z_inv * z_inv;

  const std::complex<double> num = c.b0 + c.b1 * z_inv + c.b2 * z_inv2;
  const std::complex<double> den = 1.0 + c.a1 * z_inv + c.a2 * z_inv2;

  const double mag = std::abs(num / den);
  return 20.0 * std::log10(mag);
}

double k_weighting_response_db(const KWeightingCoeffs& kw, double freq_hz, double sample_rate) {
  return biquad_response_db(kw.pre, freq_hz, sample_rate) +
         biquad_response_db(kw.rlb, freq_hz, sample_rate);
}

/// Load a BiquadCoeffsD from the JSON at data["<sr>"]["coefficients"]["<stage>"].
BiquadCoeffsD load_coeffs(const sonare::test::JsonValue& stage_node) {
  BiquadCoeffsD c;
  c.b0 = stage_node["b0"].as_number();
  c.b1 = stage_node["b1"].as_number();
  c.b2 = stage_node["b2"].as_number();
  c.a1 = stage_node["a1"].as_number();
  c.a2 = stage_node["a2"].as_number();
  return c;
}

// Coefficient tolerance: the C++ and Python compute the same Deman formula
// in IEEE 754 double, so differences are at most a few ULPs (< 1e-12).
constexpr double kCoeffTol = 1.0e-12;

// Frequency-response tolerance: < 0.5 dB for the non-48 kHz Deman path.
// In practice deviations are < 0.02 dB at mid/high frequencies; 0.5 dB is a
// safe guard against future algorithmic regressions without being too loose.
constexpr double kFreqRespTolDb = 0.5;

// 48 kHz: hardcoded coefficients vs Deman response should agree to < 0.01 dB.
constexpr double kFreqRespTol48kDb = 0.01;

}  // namespace

// ---------------------------------------------------------------------------
// Coefficient correctness: C++ Deman design vs Python reference
// ---------------------------------------------------------------------------

TEST_CASE("K-weighting coefficients match reference at 22050 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& sr_node = json["data"]["22050"];
  const BiquadCoeffsD ref_pre = load_coeffs(sr_node["coefficients"]["pre"]);
  const BiquadCoeffsD ref_rlb = load_coeffs(sr_node["coefficients"]["rlb"]);

  const auto kw = k_weighting_coefficients(22050.0);

  REQUIRE_THAT(kw.pre.b0, WithinAbs(ref_pre.b0, kCoeffTol));
  REQUIRE_THAT(kw.pre.b1, WithinAbs(ref_pre.b1, kCoeffTol));
  REQUIRE_THAT(kw.pre.b2, WithinAbs(ref_pre.b2, kCoeffTol));
  REQUIRE_THAT(kw.pre.a1, WithinAbs(ref_pre.a1, kCoeffTol));
  REQUIRE_THAT(kw.pre.a2, WithinAbs(ref_pre.a2, kCoeffTol));

  REQUIRE_THAT(kw.rlb.b0, WithinAbs(ref_rlb.b0, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b1, WithinAbs(ref_rlb.b1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b2, WithinAbs(ref_rlb.b2, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a1, WithinAbs(ref_rlb.a1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a2, WithinAbs(ref_rlb.a2, kCoeffTol));
}

TEST_CASE("K-weighting coefficients match reference at 44100 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& sr_node = json["data"]["44100"];
  const BiquadCoeffsD ref_pre = load_coeffs(sr_node["coefficients"]["pre"]);
  const BiquadCoeffsD ref_rlb = load_coeffs(sr_node["coefficients"]["rlb"]);

  const auto kw = k_weighting_coefficients(44100.0);

  REQUIRE_THAT(kw.pre.b0, WithinAbs(ref_pre.b0, kCoeffTol));
  REQUIRE_THAT(kw.pre.b1, WithinAbs(ref_pre.b1, kCoeffTol));
  REQUIRE_THAT(kw.pre.b2, WithinAbs(ref_pre.b2, kCoeffTol));
  REQUIRE_THAT(kw.pre.a1, WithinAbs(ref_pre.a1, kCoeffTol));
  REQUIRE_THAT(kw.pre.a2, WithinAbs(ref_pre.a2, kCoeffTol));

  REQUIRE_THAT(kw.rlb.b0, WithinAbs(ref_rlb.b0, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b1, WithinAbs(ref_rlb.b1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b2, WithinAbs(ref_rlb.b2, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a1, WithinAbs(ref_rlb.a1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a2, WithinAbs(ref_rlb.a2, kCoeffTol));
}

TEST_CASE("K-weighting coefficients match reference at 96000 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& sr_node = json["data"]["96000"];
  const BiquadCoeffsD ref_pre = load_coeffs(sr_node["coefficients"]["pre"]);
  const BiquadCoeffsD ref_rlb = load_coeffs(sr_node["coefficients"]["rlb"]);

  const auto kw = k_weighting_coefficients(96000.0);

  REQUIRE_THAT(kw.pre.b0, WithinAbs(ref_pre.b0, kCoeffTol));
  REQUIRE_THAT(kw.pre.b1, WithinAbs(ref_pre.b1, kCoeffTol));
  REQUIRE_THAT(kw.pre.b2, WithinAbs(ref_pre.b2, kCoeffTol));
  REQUIRE_THAT(kw.pre.a1, WithinAbs(ref_pre.a1, kCoeffTol));
  REQUIRE_THAT(kw.pre.a2, WithinAbs(ref_pre.a2, kCoeffTol));

  REQUIRE_THAT(kw.rlb.b0, WithinAbs(ref_rlb.b0, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b1, WithinAbs(ref_rlb.b1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.b2, WithinAbs(ref_rlb.b2, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a1, WithinAbs(ref_rlb.a1, kCoeffTol));
  REQUIRE_THAT(kw.rlb.a2, WithinAbs(ref_rlb.a2, kCoeffTol));
}

// ---------------------------------------------------------------------------
// Frequency response: Deman-derived path vs pyloudnorm/BS.1770 reference
// ---------------------------------------------------------------------------

TEST_CASE("K-weighting frequency response matches reference at 22050 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& resp = json["data"]["22050"]["response_db"];
  const auto kw = k_weighting_coefficients(22050.0);
  constexpr double sr = 22050.0;

  // 100 Hz: dominated by the RLB high-pass roll-off (~-1.08 dB)
  {
    const double expected = resp["100"]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw, 100.0, sr);
    CAPTURE(expected, actual);
    REQUIRE_THAT(actual, WithinAbs(expected, kFreqRespTolDb));
  }
  // 1 kHz: mid-band reference (~+0.73 dB due to shelf boost)
  {
    const double expected = resp["1000"]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw, 1000.0, sr);
    CAPTURE(expected, actual);
    REQUIRE_THAT(actual, WithinAbs(expected, kFreqRespTolDb));
  }
  // 10 kHz: within the shelf (+4.09 dB); 20 kHz is above 22050/2 = 11025 Hz
  // Nyquist, so we only test up to 10 kHz here.
  {
    const double expected = resp["10000"]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw, 10000.0, sr);
    CAPTURE(expected, actual);
    REQUIRE_THAT(actual, WithinAbs(expected, kFreqRespTolDb));
  }
}

TEST_CASE("K-weighting frequency response matches reference at 44100 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& resp = json["data"]["44100"]["response_db"];
  const auto kw = k_weighting_coefficients(44100.0);
  constexpr double sr = 44100.0;

  for (const char* freq_str : {"100", "1000", "10000", "20000"}) {
    const double freq_hz = std::stod(freq_str);
    const double expected = resp[freq_str]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw, freq_hz, sr);
    CAPTURE(freq_hz, expected, actual);
    REQUIRE_THAT(actual, WithinAbs(expected, kFreqRespTolDb));
  }
}

TEST_CASE("K-weighting frequency response matches reference at 96000 Hz", "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  const auto& resp = json["data"]["96000"]["response_db"];
  const auto kw = k_weighting_coefficients(96000.0);
  constexpr double sr = 96000.0;

  for (const char* freq_str : {"100", "1000", "10000", "20000"}) {
    const double freq_hz = std::stod(freq_str);
    const double expected = resp[freq_str]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw, freq_hz, sr);
    CAPTURE(freq_hz, expected, actual);
    REQUIRE_THAT(actual, WithinAbs(expected, kFreqRespTolDb));
  }
}

// ---------------------------------------------------------------------------
// 48 kHz: verify that hardcoded reference coefficients agree with the
// Deman-derived prediction within the tight 0.01 dB tolerance.
// ---------------------------------------------------------------------------

TEST_CASE("K-weighting 48 kHz hardcoded path agrees with Deman prediction within 0.01 dB",
          "[rt][k_weighting]") {
  const auto json = JsonReader::parse_file("tests/fixtures/k_weighting_reference.json");
  // `deman_response_db` is the response computed by Python from Deman formula
  // (stored alongside the hardcoded 48 kHz reference in the JSON).
  const auto& deman_resp = json["data"]["48000"]["deman_response_db"];
  const auto kw_48k = k_weighting_coefficients(48000.0);
  constexpr double sr = 48000.0;

  for (const char* freq_str : {"100", "1000", "10000"}) {
    const double freq_hz = std::stod(freq_str);
    const double deman_db = deman_resp[freq_str]["combined_db"].as_number();
    const double actual = k_weighting_response_db(kw_48k, freq_hz, sr);
    CAPTURE(freq_hz, deman_db, actual);
    REQUIRE_THAT(actual, WithinAbs(deman_db, kFreqRespTol48kDb));
  }
}

// ---------------------------------------------------------------------------
// Sanity checks independent of reference data
// ---------------------------------------------------------------------------

TEST_CASE("K-weighting high-pass attenuates sub-100 Hz at all common rates", "[rt][k_weighting]") {
  // The RLB high-pass (-3 dB at 38 Hz) should attenuate a 10 Hz tone heavily.
  // No reference JSON needed; we just check the sign of the response.
  for (double sr : {22050.0, 44100.0, 48000.0, 96000.0}) {
    const auto kw = k_weighting_coefficients(sr);
    const double response_10hz = k_weighting_response_db(kw, 10.0, sr);
    CAPTURE(sr, response_10hz);
    REQUIRE(response_10hz < -6.0);  // at least -6 dB at 10 Hz for all rates
  }
}

TEST_CASE("K-weighting shelf boosts above 1 kHz at all common rates", "[rt][k_weighting]") {
  // The high-shelf (+4 dB, 1681 Hz) should give a net positive gain at 10 kHz
  // (shelf boost dominates the tiny high-pass contribution at this frequency).
  for (double sr : {22050.0, 44100.0, 48000.0, 96000.0}) {
    const auto kw = k_weighting_coefficients(sr);
    const double response_10khz = k_weighting_response_db(kw, 10000.0, sr);
    CAPTURE(sr, response_10khz);
    REQUIRE(response_10khz > 3.0);  // at least +3 dB at 10 kHz
    REQUIRE(response_10khz < 5.0);  // shelf gain is ~+4 dB, not more than +5 dB
  }
}

TEST_CASE("K-weighting response is continuous across sample-rate boundary near 48 kHz",
          "[rt][k_weighting]") {
  // The function returns exact hardcoded coefficients at exactly 48000.0 Hz
  // and uses Deman for anything else.  The two paths must agree at representative
  // frequencies to within 0.01 dB (ensures there is no discontinuity at the
  // boundary due to e.g. rounding in the hardcoded constants).
  const auto kw_exact = k_weighting_coefficients(48000.0);   // hardcoded path
  const auto kw_nearby = k_weighting_coefficients(48000.1);  // Deman path, essentially 48 kHz

  for (double freq_hz : {100.0, 1000.0, 10000.0}) {
    const double resp_exact = k_weighting_response_db(kw_exact, freq_hz, 48000.0);
    const double resp_nearby = k_weighting_response_db(kw_nearby, freq_hz, 48000.1);
    CAPTURE(freq_hz, resp_exact, resp_nearby);
    REQUIRE_THAT(resp_exact, WithinAbs(resp_nearby, 0.01));
  }
}
