/// @file iir_test.cpp
/// @brief Tests for IIR filter implementation.

#include "filters/iir.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "rt/biquad_design.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::constants::kTwoPi;
using sonare::test::generate_sine;

void require_coeffs_close(const BiquadCoeffs& actual, const rt::BiquadCoeffs& expected,
                          float tolerance = 1.0e-6f) {
  REQUIRE_THAT(actual.b0, WithinAbs(expected.b0, tolerance));
  REQUIRE_THAT(actual.b1, WithinAbs(expected.b1, tolerance));
  REQUIRE_THAT(actual.b2, WithinAbs(expected.b2, tolerance));
  REQUIRE_THAT(actual.a1, WithinAbs(expected.a1, tolerance));
  REQUIRE_THAT(actual.a2, WithinAbs(expected.a2, tolerance));
}
}  // namespace

TEST_CASE("lowpass_coeffs valid range", "[iir]") {
  auto coeffs = lowpass_coeffs(1000.0f, 22050);

  // Coefficients should be finite
  REQUIRE(std::isfinite(coeffs.b0));
  REQUIRE(std::isfinite(coeffs.b1));
  REQUIRE(std::isfinite(coeffs.b2));
  REQUIRE(std::isfinite(coeffs.a1));
  REQUIRE(std::isfinite(coeffs.a2));
}

TEST_CASE("IIR coefficient helpers match shared RBJ biquad designs", "[iir]") {
  constexpr int sr = 48000;
  constexpr float cutoff = 1200.0f;
  constexpr float center = 2400.0f;
  constexpr float bandwidth = 600.0f;
  constexpr float q = center / bandwidth;

  require_coeffs_close(
      lowpass_coeffs(cutoff, sr),
      rt::rbj_lowpass(kTwoPi * cutoff / static_cast<float>(sr), sonare::constants::kButterworthQ));
  require_coeffs_close(
      highpass_coeffs(cutoff, sr),
      rt::rbj_highpass(kTwoPi * cutoff / static_cast<float>(sr), sonare::constants::kButterworthQ));
  require_coeffs_close(bandpass_coeffs(center, bandwidth, sr),
                       rt::rbj_bandpass(kTwoPi * center / static_cast<float>(sr), q));
  require_coeffs_close(notch_coeffs(center, bandwidth, sr),
                       rt::rbj_notch(kTwoPi * center / static_cast<float>(sr), q));
}

TEST_CASE("highpass_coeffs valid range", "[iir]") {
  auto coeffs = highpass_coeffs(1000.0f, 22050);

  REQUIRE(std::isfinite(coeffs.b0));
  REQUIRE(std::isfinite(coeffs.b1));
  REQUIRE(std::isfinite(coeffs.b2));
  REQUIRE(std::isfinite(coeffs.a1));
  REQUIRE(std::isfinite(coeffs.a2));
}

TEST_CASE("lowpass attenuates high frequencies", "[iir]") {
  int sr = 22050;
  int samples = sr;  // 1 second
  float cutoff = 1000.0f;

  // Generate signals at different frequencies
  auto low_freq = generate_sine(samples, 200.0f, sr);    // Below cutoff
  auto high_freq = generate_sine(samples, 5000.0f, sr);  // Above cutoff

  auto coeffs = lowpass_coeffs(cutoff, sr);

  // Apply filter
  auto filtered_low = apply_biquad(low_freq, coeffs);
  auto filtered_high = apply_biquad(high_freq, coeffs);

  // Compute RMS after transient (skip first 1000 samples)
  auto rms_after_transient = [](const std::vector<float>& signal, int skip) {
    float sum_sq = 0.0f;
    int count = 0;
    for (size_t i = skip; i < signal.size(); ++i) {
      sum_sq += signal[i] * signal[i];
      count++;
    }
    return std::sqrt(sum_sq / count);
  };

  float rms_low_input = rms_after_transient(low_freq, 1000);
  float rms_low_output = rms_after_transient(filtered_low, 1000);
  float rms_high_input = rms_after_transient(high_freq, 1000);
  float rms_high_output = rms_after_transient(filtered_high, 1000);

  // Low frequency should pass through mostly unchanged
  float low_ratio = rms_low_output / rms_low_input;
  REQUIRE(low_ratio > 0.7f);
  REQUIRE(low_ratio < 1.3f);

  // High frequency should be significantly attenuated
  float high_ratio = rms_high_output / rms_high_input;
  REQUIRE(high_ratio < 0.3f);
}

TEST_CASE("highpass attenuates low frequencies", "[iir]") {
  int sr = 22050;
  int samples = sr;
  float cutoff = 1000.0f;

  auto low_freq = generate_sine(samples, 100.0f, sr);    // Below cutoff
  auto high_freq = generate_sine(samples, 5000.0f, sr);  // Above cutoff

  auto coeffs = highpass_coeffs(cutoff, sr);

  auto filtered_low = apply_biquad(low_freq, coeffs);
  auto filtered_high = apply_biquad(high_freq, coeffs);

  auto rms_after_transient = [](const std::vector<float>& signal, int skip) {
    float sum_sq = 0.0f;
    int count = 0;
    for (size_t i = skip; i < signal.size(); ++i) {
      sum_sq += signal[i] * signal[i];
      count++;
    }
    return std::sqrt(sum_sq / count);
  };

  float rms_low_input = rms_after_transient(low_freq, 1000);
  float rms_low_output = rms_after_transient(filtered_low, 1000);
  float rms_high_input = rms_after_transient(high_freq, 1000);
  float rms_high_output = rms_after_transient(filtered_high, 1000);

  // Low frequency should be significantly attenuated
  float low_ratio = rms_low_output / rms_low_input;
  REQUIRE(low_ratio < 0.3f);

  // High frequency should pass through mostly unchanged
  float high_ratio = rms_high_output / rms_high_input;
  REQUIRE(high_ratio > 0.7f);
  REQUIRE(high_ratio < 1.3f);
}

TEST_CASE("bandpass coefficients", "[iir]") {
  auto coeffs = bandpass_coeffs(1000.0f, 500.0f, 22050);

  REQUIRE(std::isfinite(coeffs.b0));
  REQUIRE(std::isfinite(coeffs.b1));
  REQUIRE(std::isfinite(coeffs.b2));
  REQUIRE(std::isfinite(coeffs.a1));
  REQUIRE(std::isfinite(coeffs.a2));
}

TEST_CASE("notch coefficients", "[iir]") {
  auto coeffs = notch_coeffs(60.0f, 10.0f, 22050);  // 60 Hz hum removal

  REQUIRE(std::isfinite(coeffs.b0));
  REQUIRE(std::isfinite(coeffs.b1));
  REQUIRE(std::isfinite(coeffs.b2));
  REQUIRE(std::isfinite(coeffs.a1));
  REQUIRE(std::isfinite(coeffs.a2));
}

TEST_CASE("apply_biquad_filtfilt zero phase", "[iir]") {
  int sr = 22050;
  int samples = sr / 2;  // 0.5 seconds

  // Generate a signal with a sharp transient
  std::vector<float> signal(samples, 0.0f);
  signal[samples / 2] = 1.0f;  // Impulse in the middle

  auto coeffs = lowpass_coeffs(1000.0f, sr);

  auto filtered_forward = apply_biquad(signal, coeffs);
  auto filtered_filtfilt = apply_biquad_filtfilt(signal.data(), signal.size(), coeffs);

  // Find peak position in both
  int peak_forward = 0;
  int peak_filtfilt = 0;
  float max_forward = 0.0f;
  float max_filtfilt = 0.0f;

  for (int i = 0; i < samples; ++i) {
    if (filtered_forward[i] > max_forward) {
      max_forward = filtered_forward[i];
      peak_forward = i;
    }
    if (filtered_filtfilt[i] > max_filtfilt) {
      max_filtfilt = filtered_filtfilt[i];
      peak_filtfilt = i;
    }
  }

  // Forward filter has delay
  REQUIRE(peak_forward > samples / 2);

  // filtfilt should preserve peak position (zero phase)
  REQUIRE(std::abs(peak_filtfilt - samples / 2) < 5);
}

TEST_CASE("apply_biquad_filtfilt reduced edge transient", "[iir]") {
  // A lowpass filtfilt of a DC (constant) signal should pass it through almost
  // unchanged everywhere. With zero initial conditions the steady-state seeding
  // is what removes the large boundary transient at the first samples; assert the
  // edges track the constant level closely.
  const int sr = 22050;
  const int samples = 4096;
  const float level = 1.0f;
  std::vector<float> signal(samples, level);

  auto coeffs = lowpass_coeffs(1000.0f, sr);
  auto filtered = apply_biquad_filtfilt(signal.data(), signal.size(), coeffs);

  // The very first samples (the historically transient-prone region) must be
  // close to the DC level, not ramping up from zero.
  REQUIRE_THAT(filtered[0], WithinAbs(level, 0.02f));
  REQUIRE_THAT(filtered[1], WithinAbs(level, 0.02f));
  REQUIRE_THAT(filtered[samples - 1], WithinAbs(level, 0.02f));

  // Edge deviation must be no worse than the steady interior deviation by much.
  const float mid = filtered[samples / 2];
  REQUIRE(std::abs(filtered[0] - mid) < 0.02f);
  REQUIRE(std::abs(filtered[samples - 1] - mid) < 0.02f);
}

TEST_CASE("apply_biquad empty input", "[iir]") {
  auto coeffs = lowpass_coeffs(1000.0f, 22050);
  std::vector<float> empty;
  auto result = apply_biquad(empty, coeffs);
  REQUIRE(result.empty());
}
