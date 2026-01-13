/// @file iir_test.cpp
/// @brief Tests for IIR filter implementation.

#include "filters/iir.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
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

TEST_CASE("apply_biquad empty input", "[iir]") {
  auto coeffs = lowpass_coeffs(1000.0f, 22050);
  std::vector<float> empty;
  auto result = apply_biquad(empty, coeffs);
  REQUIRE(result.empty());
}
