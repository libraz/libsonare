/// @file vowel_filter_test.cpp
/// @brief Vowel filter: table, fit reproduction, log-domain glide, drive and factory wiring.

#include "effects/filter/vowel_filter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "rt/adaa.h"
#include "rt/nonlinearities.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

TEST_CASE("vowel drive switch accepts only boolean automation values", "[effects][vowel][switch]") {
  sonare::effects::filter::VowelFilter filter;
  CHECK(filter.set_parameter(3, 0.0f));
  CHECK(filter.set_parameter(3, 1.0f));
  for (float value : {-1.0f, 0.5f, 2.0f}) {
    INFO("driveOn=" << value);
    CHECK_FALSE(filter.set_parameter(3, value));
  }
}

namespace {

using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;
using sonare::effects::filter::kVowelBandCount;
using sonare::effects::filter::kVowelCount;
using sonare::effects::filter::vowel_table_hz;
using sonare::effects::filter::VowelFilter;
using sonare::effects::filter::VowelFilterConfig;

constexpr double kRate = 48000.0;
constexpr int kIrLength = 8192;

// The measured fit, spelled independently of the implementation: three 20 dB
// peaking sections summed with signed weights beside a direct path, plus 3.064 dB.
constexpr double kFitHz[3] = {1003.972, 1312.844, 1903.283};
constexpr double kFitQ[3] = {1.582894, 0.764481, 2.161686};
constexpr double kFitWeight[3] = {0.162137595, -0.027334462, 0.080888403};
constexpr double kFitDirect = -0.150716821;
constexpr double kFitConstantDb = 3.064;

// Japanese male means (Hz) in printed order a i u e o.
constexpr double kJapanese[5][3] = {
    {687, 1283, 2605}, {301, 2154, 2929}, {348, 1435, 2355}, {443, 1947, 2611}, {462, 949, 2544}};

std::complex<double> peaking(double f0, double q, double w) {
  const double amp = std::pow(10.0, 20.0 / 40.0);
  const double w0 = kTwoPiD * f0 / kRate;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double c = std::cos(w0);
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = z1 * z1;
  const std::complex<double> num = (1 + alpha * amp) - 2 * c * z1 + (1 - alpha * amp) * z2;
  const std::complex<double> den = (1 + alpha / amp) - 2 * c * z1 + (1 - alpha / amp) * z2;
  return num / den;
}

/// The fit's magnitude (dB) at @p hz with the three peak frequencies replaced by @p centres.
double fit_db(const double centres[3], double hz) {
  const double w = kTwoPiD * hz / kRate;
  std::complex<double> sum = kFitDirect;
  for (int b = 0; b < 3; ++b) sum += kFitWeight[b] * peaking(centres[b], kFitQ[b], w);
  return 20.0 * std::log10(std::abs(sum)) + kFitConstantDb;
}

/// Magnitude (dB) at @p hz of an impulse response, by direct DFT.
double ir_db(const std::vector<float>& ir, double hz) {
  std::complex<double> sum = 0.0;
  const double step = kTwoPiD * hz / kRate;
  for (std::size_t n = 0; n < ir.size(); ++n) {
    sum += static_cast<double>(ir[n]) * std::polar(1.0, -step * static_cast<double>(n));
  }
  return 20.0 * std::log10(std::abs(sum) + 1e-30);
}

std::vector<double> probe_hz() {
  std::vector<double> out;
  for (int i = 0; i < 48; ++i) out.push_back(100.0 * std::pow(80.0, i / 47.0));
  return out;
}

std::vector<float> impulse_response(VowelFilter& filter, int length = kIrLength) {
  std::vector<float> ir(static_cast<std::size_t>(length), 0.0f);
  ir[0] = 1.0f;
  sonare::test::process(filter, ir);
  return ir;
}

VowelFilterConfig settled(float vowel) {
  VowelFilterConfig config;
  config.vowel = vowel;
  config.accel_ms = 0.0f;
  return config;
}

double rms_db_error(const std::vector<float>& ir, const double centres[3]) {
  double sum = 0.0;
  const std::vector<double> freqs = probe_hz();
  for (const double hz : freqs) {
    const double d = ir_db(ir, hz) - fit_db(centres, hz);
    sum += d * d;
  }
  return std::sqrt(sum / static_cast<double>(freqs.size()));
}

}  // namespace

TEST_CASE("Vowel table: the measured vowel is the fit and the rest carry its triple",
          "[vowel-filter]") {
  for (int b = 0; b < kVowelBandCount; ++b) {
    CHECK(vowel_table_hz(0, b) == Catch::Approx(kFitHz[b]).margin(1e-3));
    for (int v = 1; v < kVowelCount; ++v) {
      const double expected = kFitHz[b] * kJapanese[v][b] / kJapanese[0][b];
      CAPTURE(v, b);
      CHECK(vowel_table_hz(v, b) == Catch::Approx(expected).epsilon(1e-5));
    }
  }
  // Every vowel is a different triple.
  for (int u = 0; u < kVowelCount; ++u) {
    for (int v = u + 1; v < kVowelCount; ++v) {
      double spread = 0.0;
      for (int b = 0; b < kVowelBandCount; ++b) {
        spread += std::fabs(std::log(vowel_table_hz(u, b) / vowel_table_hz(v, b)));
      }
      CAPTURE(u, v);
      CHECK(spread > 0.2);
    }
  }
}

TEST_CASE("Vowel filter: vowel 0 reproduces the fit's magnitude response", "[vowel-filter]") {
  VowelFilter filter(settled(0.0f));
  filter.prepare(kRate, kIrLength);
  const std::vector<float> ir = impulse_response(filter);
  double worst = 0.0;
  for (const double hz : probe_hz()) {
    worst = std::max(worst, std::fabs(ir_db(ir, hz) - fit_db(kFitHz, hz)));
  }
  CAPTURE(worst);
  CHECK(worst < 0.05);
}

TEST_CASE("Vowel filter: every vowel's peaks land at the table's frequencies", "[vowel-filter]") {
  for (int v = 0; v < kVowelCount; ++v) {
    VowelFilter filter(settled(static_cast<float>(v)));
    filter.prepare(kRate, kIrLength);
    const std::vector<float> ir = impulse_response(filter);
    double centres[3];
    for (int b = 0; b < 3; ++b) centres[b] = vowel_table_hz(v, b);
    CAPTURE(v);
    CHECK(rms_db_error(ir, centres) < 0.05);

    // The first formant is a local maximum of the measured response at the table's F1.
    const double f1 = centres[0];
    const double at = ir_db(ir, f1);
    CHECK(at > ir_db(ir, f1 * 0.8));
    CHECK(at > ir_db(ir, f1 * 1.25));
  }
}

TEST_CASE("Vowel filter: each vowel sounds different", "[vowel-filter]") {
  std::vector<std::vector<float>> responses;
  for (int v = 0; v < kVowelCount; ++v) {
    VowelFilter filter(settled(static_cast<float>(v)));
    filter.prepare(kRate, kIrLength);
    responses.push_back(impulse_response(filter));
  }
  for (int u = 0; u < kVowelCount; ++u) {
    for (int v = u + 1; v < kVowelCount; ++v) {
      double worst = 0.0;
      for (const double hz : probe_hz()) {
        worst = std::max(worst, std::fabs(ir_db(responses[static_cast<std::size_t>(u)], hz) -
                                          ir_db(responses[static_cast<std::size_t>(v)], hz)));
      }
      CAPTURE(u, v, worst);
      CHECK(worst > 1.0);
    }
  }
}

TEST_CASE("Vowel filter: a fractional vowel interpolates in log frequency", "[vowel-filter]") {
  VowelFilter filter(settled(0.5f));
  filter.prepare(kRate, kIrLength);
  const std::vector<float> ir = impulse_response(filter);
  double geometric[3];
  double arithmetic[3];
  for (int b = 0; b < 3; ++b) {
    geometric[b] = std::sqrt(vowel_table_hz(0, b) * vowel_table_hz(1, b));
    arithmetic[b] = 0.5 * (vowel_table_hz(0, b) + vowel_table_hz(1, b));
  }
  CHECK(rms_db_error(ir, geometric) < 0.05);
  CHECK(rms_db_error(ir, arithmetic) > 0.5);
}

TEST_CASE("Vowel filter: accel glides the triple in the log domain, not along the vowels",
          "[vowel-filter]") {
  constexpr float kTauMs = 1000.0f;
  VowelFilterConfig config = settled(0.0f);
  config.accel_ms = kTauMs;
  VowelFilter filter(config);
  filter.prepare(kRate, 4096);
  std::vector<float> quiet(static_cast<std::size_t>(kRate), 0.0f);
  sonare::test::process(filter, quiet);
  REQUIRE(filter.set_parameter(0, 2.0f));  // a -> u, past i.

  // One time constant times ln 2 leaves the log frequency halfway to the target.
  std::vector<float> wait(static_cast<std::size_t>(kRate * kTauMs * 1e-3 * std::log(2.0)), 0.0f);
  sonare::test::process(filter, wait);
  const std::vector<float> ir = impulse_response(filter, 2048);

  double geometric[3];
  double arithmetic[3];
  double at_i[3];
  for (int b = 0; b < 3; ++b) {
    geometric[b] = std::sqrt(vowel_table_hz(0, b) * vowel_table_hz(2, b));
    arithmetic[b] = 0.5 * (vowel_table_hz(0, b) + vowel_table_hz(2, b));
    at_i[b] = vowel_table_hz(1, b);
  }
  const double geo = rms_db_error(ir, geometric);
  const double lin = rms_db_error(ir, arithmetic);
  const double via_i = rms_db_error(ir, at_i);
  CAPTURE(geo, lin, via_i);
  CHECK(geo < 0.5);
  CHECK(geo < lin);
  CHECK(geo < via_i);

  // Given long enough it lands on the target.
  std::vector<float> more(static_cast<std::size_t>(kRate * 8.0), 0.0f);
  sonare::test::process(filter, more);
  const std::vector<float> settled_ir = impulse_response(filter);
  double target[3];
  for (int b = 0; b < 3; ++b) target[b] = vowel_table_hz(2, b);
  CHECK(rms_db_error(settled_ir, target) < 0.05);
}

TEST_CASE("Vowel filter: the drive is an antiderivative-antialiased tanh ahead of the bank",
          "[vowel-filter]") {
  constexpr float kDrive = 0.5f;
  constexpr float kDriveMaxDb = 36.0f;
  const float gain = std::pow(10.0f, kDrive * kDriveMaxDb / 20.0f);
  const int n = 4800;
  std::vector<float> input = sonare::test::generate_sine(n, 5000.0f, static_cast<int>(kRate), 0.5f);

  VowelFilterConfig driven = settled(0.0f);
  driven.drive = kDrive;
  driven.drive_on = true;
  VowelFilter with_drive(driven);
  with_drive.prepare(kRate, n);
  std::vector<float> out = input;
  sonare::test::process(with_drive, out);

  // The same bank fed a pre-shaped signal must match sample for sample.
  sonare::rt::Adaa1<sonare::rt::TanhNonlinearity> adaa;
  adaa.reset(gain * input[0]);
  const float post = 1.0f / std::tanh(gain);
  std::vector<float> shaped(input.size());
  std::vector<float> plain(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    shaped[i] = post * adaa.process(gain * input[i]);
    plain[i] = post * std::tanh(gain * input[i]);
  }
  VowelFilter bank_only(settled(0.0f));
  bank_only.prepare(kRate, n);
  std::vector<float> reference = shaped;
  sonare::test::process(bank_only, reference);
  CHECK(sonare::test::max_abs_difference(out, reference) < 1e-5f);

  // And it is not the plain tanh: the antialiasing changes the signal.
  VowelFilter bank_plain(settled(0.0f));
  bank_plain.prepare(kRate, n);
  std::vector<float> plain_out = plain;
  sonare::test::process(bank_plain, plain_out);
  CHECK(sonare::test::max_abs_difference(out, plain_out) > 1e-4f);

  // The drive saturates: a 4x larger input does not give a 4x larger output.
  std::vector<float> loud = input;
  for (float& x : loud) x *= 4.0f;
  VowelFilter loud_filter(driven);
  loud_filter.prepare(kRate, n);
  sonare::test::process(loud_filter, loud);
  CHECK(sonare::test::peak_abs(loud, 480) < 3.0f * sonare::test::peak_abs(out, 480));

  // With the drive off the bank is linear.
  VowelFilter linear_a(settled(0.0f));
  VowelFilter linear_b(settled(0.0f));
  linear_a.prepare(kRate, n);
  linear_b.prepare(kRate, n);
  std::vector<float> a = input;
  std::vector<float> b = input;
  for (float& x : b) x *= 2.0f;
  sonare::test::process(linear_a, a);
  sonare::test::process(linear_b, b);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] *= 2.0f;
  CHECK(sonare::test::max_abs_difference(a, b) < 1e-5f);
}

TEST_CASE("Vowel filter: dry/wet and the factory keys", "[vowel-filter]") {
  using sonare::mastering::api::make_insert;
  auto dry = make_insert("effects.filter.vowel", R"({"dryWet":0})");
  REQUIRE(dry != nullptr);
  dry->prepare(kRate, 512);
  std::vector<float> signal =
      sonare::test::generate_sine(512, 440.0f, static_cast<int>(kRate), 0.5f);
  std::vector<float> processed = signal;
  sonare::test::process(*dry, processed);
  CHECK(processed == signal);

  auto full = make_insert("effects.filter.vowel",
                          R"({"vowel":2.5,"accelMs":30,"drive":0.7,"driveOn":true,"dryWet":0.8})");
  REQUIRE(full != nullptr);
  const auto descriptors = full->parameter_descriptors();
  REQUIRE(descriptors.size() == 5);
  CHECK(descriptors[0].key == std::string("vowel"));
  CHECK(descriptors[3].key == std::string("driveOn"));
  CHECK(full->set_parameter(0, 3.0f));
  CHECK(full->set_parameter(3, 1.0f));
  CHECK_FALSE(full->set_parameter(0, std::numeric_limits<float>::quiet_NaN()));
  CHECK_FALSE(full->set_parameter(5, 1.0f));
}

TEST_CASE("Vowel filter: recovers after a non-finite sample", "[vowel-filter]") {
  VowelFilter filter(settled(1.0f));
  filter.prepare(kRate, 512);
  std::vector<float> bad(256, 0.1f);
  bad[10] = std::numeric_limits<float>::quiet_NaN();
  sonare::test::process(filter, bad);
  std::vector<float> after =
      sonare::test::generate_sine(512, 300.0f, static_cast<int>(kRate), 0.3f);
  sonare::test::process(filter, after);
  for (const float x : after) REQUIRE(std::isfinite(x));
  CHECK(filter.non_finite_discard_count() > 0);
}
