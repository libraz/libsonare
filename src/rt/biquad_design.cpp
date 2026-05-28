#include "rt/biquad_design.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::rt {

using sonare::constants::kButterworthQ;
using sonare::constants::kHalfPi;
using sonare::constants::kPi;
using sonare::constants::kPiD;

namespace {

struct PoleCoeffs {
  float a1 = 0.0f;
  float a2 = 0.0f;
  float A0 = 0.0f;
  float A1 = 0.0f;
  float A2 = 0.0f;
  float p0 = 0.0f;
  float p1 = 0.0f;
  float p2 = 0.0f;
};

// RT-safe: coefficient recomputation may run on the audio thread (e.g. on a
// parameter change), so this must never throw out of a noexcept callback.
// A non-positive Q is clamped to a small positive value instead.
float checked_q(float q) noexcept {
  if (!(q > 0.0f) || !std::isfinite(q)) {
    return 1.0e-6f;
  }
  return std::max(q, 1.0e-6f);
}

float safe_sqrt(float x) { return std::sqrt(std::max(0.0f, x)); }

float safe_div(float numerator, float denominator, float fallback) {
  return std::abs(denominator) > 1.0e-12f ? numerator / denominator : fallback;
}

// RT-safe: never throws. A degenerate a0 (zero or non-finite) cannot be
// normalized, so fall back to a unity-gain passthrough biquad
// (b0=1, all other taps 0) rather than terminating a noexcept callback.
BiquadCoeffs normalize(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
  if (!(std::abs(a0) > 0.0) || !std::isfinite(a0)) {
    return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  }

  const double inv_a0 = 1.0 / a0;
  return {
      static_cast<float>(b0 * inv_a0), static_cast<float>(b1 * inv_a0),
      static_cast<float>(b2 * inv_a0), static_cast<float>(a1 * inv_a0),
      static_cast<float>(a2 * inv_a0),
  };
}

float magnitude_at(const BiquadCoeffs& coeffs, float omega) {
  const std::complex<float> z1 = std::exp(std::complex<float>(0.0f, -omega));
  const std::complex<float> z2 = z1 * z1;
  const std::complex<float> numerator = coeffs.b0 + coeffs.b1 * z1 + coeffs.b2 * z2;
  const std::complex<float> denominator = 1.0f + coeffs.a1 * z1 + coeffs.a2 * z2;
  const float denom = std::abs(denominator);
  if (denom <= 1.0e-12f) {
    return 0.0f;
  }
  return std::abs(numerator) / denom;
}

bool endpoint_gain_error_exceeds(const BiquadCoeffs& coeffs, float omega, float target_gain_db,
                                 float tolerance_db) {
  const float magnitude = magnitude_at(coeffs, omega);
  if (!(magnitude > 0.0f) || !std::isfinite(magnitude)) {
    return true;
  }
  const float actual_gain_db = linear_to_db(magnitude);
  return std::abs(actual_gain_db - target_gain_db) > tolerance_db;
}

PoleCoeffs matched_poles(float w0, float q_value) {
  const float q = 0.5f / checked_q(q_value);
  const float expmqw = std::exp(-q * w0);
  PoleCoeffs p;
  if (q <= 1.0f) {
    p.a1 = -2.0f * expmqw * std::cos(std::sqrt(std::max(0.0f, 1.0f - q * q)) * w0);
  } else {
    p.a1 = -2.0f * expmqw * std::cosh(std::sqrt(q * q - 1.0f) * w0);
  }
  p.a2 = expmqw * expmqw;
  const float sinpd2 = std::sin(0.5f * w0);
  p.p1 = std::max(sinpd2 * sinpd2, 1.0e-8f);
  p.p0 = std::max(1.0f - p.p1, 1.0e-8f);
  p.p2 = 4.0f * p.p0 * p.p1;
  p.A0 = (1.0f + p.a1 + p.a2) * (1.0f + p.a1 + p.a2);
  p.A1 = (1.0f - p.a1 + p.a2) * (1.0f - p.a1 + p.a2);
  p.A2 = -4.0f * p.a2;
  return p;
}

}  // namespace

BiquadCoeffs rbj_lowpass(float w0, float q) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  return normalize((1.0 - cos_w0) * 0.5, 1.0 - cos_w0, (1.0 - cos_w0) * 0.5, 1.0 + alpha,
                   -2.0 * cos_w0, 1.0 - alpha);
}

BiquadCoeffs rbj_highpass(float w0, float q) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  return normalize((1.0 + cos_w0) * 0.5, -(1.0 + cos_w0), (1.0 + cos_w0) * 0.5, 1.0 + alpha,
                   -2.0 * cos_w0, 1.0 - alpha);
}

BiquadCoeffs first_order_lowpass(float w0) {
  const double k = std::tan(static_cast<double>(w0) * 0.5);
  const double inv = 1.0 / (1.0 + k);
  const float b0 = static_cast<float>(k * inv);
  return {b0, b0, 0.0f, static_cast<float>((k - 1.0) * inv), 0.0f};
}

BiquadCoeffs first_order_highpass(float w0) {
  const double k = std::tan(static_cast<double>(w0) * 0.5);
  const double inv = 1.0 / (1.0 + k);
  const float b0 = static_cast<float>(inv);
  return {b0, -b0, 0.0f, static_cast<float>((k - 1.0) * inv), 0.0f};
}

BiquadCoeffs rbj_bandpass(float w0, float q) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  return normalize(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha);
}

BiquadCoeffs rbj_notch(float w0, float q) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  return normalize(1.0, -2.0 * cos_w0, 1.0, 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha);
}

BiquadCoeffs rbj_peak(float w0, float q, float gain_db) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  const double a = std::pow(10.0, static_cast<double>(gain_db) / 40.0);
  return normalize(1.0 + alpha * a, -2.0 * cos_w0, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cos_w0,
                   1.0 - alpha / a);
}

BiquadCoeffs rbj_low_shelf(float w0, float q, float gain_db) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  const double a = std::pow(10.0, static_cast<double>(gain_db) / 40.0);
  const double sqrt_a = std::sqrt(a);
  const double two_sqrt_a_alpha = 2.0 * sqrt_a * alpha;
  return normalize(a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha),
                   2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0),
                   a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha),
                   (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha,
                   -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0),
                   (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
}

BiquadCoeffs rbj_high_shelf(float w0, float q, float gain_db) {
  const double q_value = checked_q(q);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double alpha = sin_w0 / (2.0 * q_value);
  const double a = std::pow(10.0, static_cast<double>(gain_db) / 40.0);
  const double sqrt_a = std::sqrt(a);
  const double two_sqrt_a_alpha = 2.0 * sqrt_a * alpha;
  return normalize(a * ((a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha),
                   -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0),
                   a * ((a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha),
                   (a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha,
                   2.0 * ((a - 1.0) - (a + 1.0) * cos_w0),
                   (a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
}

BiquadCoeffs vicanek_lowpass(float w0, float q) {
  const PoleCoeffs p = matched_poles(w0, q);
  const float R1 = (p.A0 * p.p0 + p.A1 * p.p1 + p.A2 * p.p2) * checked_q(q) * checked_q(q);
  const float B0 = p.A0;
  const float B1 = (R1 - B0 * p.p0) / p.p1;
  const float b0 = 0.5f * (safe_sqrt(B0) + safe_sqrt(B1));
  const float b1 = safe_sqrt(B0) - b0;
  return {b0, b1, 0.0f, p.a1, p.a2};
}

BiquadCoeffs vicanek_highpass(float w0, float q) {
  const PoleCoeffs p = matched_poles(w0, q);
  const float b0 =
      safe_sqrt(p.A0 * p.p0 + p.A1 * p.p1 + p.A2 * p.p2) * checked_q(q) / (4.0f * p.p1);
  return {b0, -2.0f * b0, b0, p.a1, p.a2};
}

BiquadCoeffs vicanek_bandpass(float w0, float q) {
  const PoleCoeffs p = matched_poles(w0, q);
  const float R1 = p.A0 * p.p0 + p.A1 * p.p1 + p.A2 * p.p2;
  const float R2 = -p.A0 + p.A1 + 4.0f * (p.p0 - p.p1) * p.A2;
  const float B2 = (R1 - R2 * p.p1) / (4.0f * p.p1 * p.p1);
  const float B1 = R2 + 4.0f * (p.p1 - p.p0) * B2;
  const float b1 = -0.5f * safe_sqrt(B1);
  const float b0 = 0.5f * (safe_sqrt(B2 + 0.25f * B1) - b1);
  return {b0, b1, -b0 - b1, p.a1, p.a2};
}

BiquadCoeffs vicanek_notch(float w0, float q) {
  const PoleCoeffs p = matched_poles(w0, q);
  float b0 = 1.0f;
  float b1 = -2.0f * std::cos(w0);
  float b2 = 1.0f;
  const float denom = b0 + b1 + b2;
  const float scale = std::abs(denom) > 1.0e-8f ? safe_sqrt(p.A0) / denom : 1.0f;
  return {b0 * scale, b1 * scale, b2 * scale, p.a1, p.a2};
}

BiquadCoeffs vicanek_peak(float w0, float q, float gain_db) {
  const float G = std::pow(10.0f, gain_db / 40.0f);
  const float pole_q = std::max(q / std::max(G, 1.0e-6f), 1.0e-6f);
  const PoleCoeffs p = matched_poles(w0, pole_q);
  const float G2 = G * G * G * G;
  const float R1 = (p.A0 * p.p0 + p.A1 * p.p1 + p.A2 * p.p2) * G2;
  const float R2 = (-p.A0 + p.A1 + 4.0f * (p.p0 - p.p1) * p.A2) * G2;
  const float B0 = p.A0;
  const float B2 = (R1 - R2 * p.p1 - B0) / (4.0f * p.p1 * p.p1);
  const float B1 = R2 + B0 + 4.0f * (p.p1 - p.p0) * B2;
  const float W = 0.5f * (safe_sqrt(B0) + safe_sqrt(B1));
  const float b0 = 0.5f * (W + safe_sqrt(W * W + B2));
  const float b1 = 0.5f * (safe_sqrt(B0) - safe_sqrt(B1));
  const float b2 = std::abs(b0) > 1.0e-8f ? -B2 / (4.0f * b0) : 0.0f;
  return {b0, b1, b2, p.a1, p.a2};
}

BiquadCoeffs vicanek_high_shelf(float w0, float gain_db) {
  // Vicanek, "Matched Two-Pole Digital Shelving Filters" (2024/2025),
  // Appendix A.1 pseudo-code for the matched Butterworth high shelf.
  const float gain = db_to_linear(gain_db);
  if (std::abs(gain - 1.0f) < 1.0e-6f) {
    return {};
  }
  const float fc = std::clamp(w0 / kPi, 1.0e-6f, 4.0f);
  const float g = gain;
  const float invg = 1.0f / g;
  const float fc2 = fc * fc;
  const float fc4 = fc2 * fc2;
  const float hny = (fc4 + g) / (fc4 + invg);

  const float f1 = fc / std::sqrt(0.160f + 1.543f * fc2);
  const float f14 = f1 * f1 * f1 * f1;
  const float h1 = (fc4 + f14 * g) / (fc4 + f14 * invg);
  const float phi1 = std::pow(std::sin(kHalfPi * f1), 2.0f);

  const float f2 = fc / std::sqrt(0.947f + 3.806f * fc2);
  const float f24 = f2 * f2 * f2 * f2;
  const float h2 = (fc4 + f24 * g) / (fc4 + f24 * invg);
  const float phi2 = std::pow(std::sin(kHalfPi * f2), 2.0f);

  const float d1 = (h1 - 1.0f) * (1.0f - phi1);
  const float c11 = -phi1 * d1;
  const float c12 = phi1 * phi1 * (hny - h1);
  const float d2 = (h2 - 1.0f) * (1.0f - phi2);
  const float c21 = -phi2 * d2;
  const float c22 = phi2 * phi2 * (hny - h2);
  const float denom = c11 * c22 - c12 * c21;
  const float alfa1 = safe_div(c22 * d1 - c12 * d2, denom, 0.0f);
  const float aa1 = safe_div(d1 - c11 * alfa1, c12, 1.0f);
  const float bb1 = hny * aa1;
  const float aa2 = 0.25f * (alfa1 - aa1);
  const float bb2 = 0.25f * (alfa1 - bb1);

  const float v = 0.5f * (1.0f + safe_sqrt(aa1));
  const float w = 0.5f * (1.0f + safe_sqrt(bb1));
  const float a0 = 0.5f * (v + safe_sqrt(v * v + aa2));
  const float inva0 = safe_div(1.0f, a0, 1.0f);
  const float b0_unscaled = 0.5f * (w + safe_sqrt(w * w + bb2));
  const float b0 = b0_unscaled * inva0;
  const BiquadCoeffs coeffs{b0, (1.0f - w) * inva0,
                            -0.25f * bb2 * inva0 * inva0 / std::max(b0_unscaled, 1.0e-12f),
                            (1.0f - v) * inva0, -0.25f * aa2 * inva0 * inva0};
  if (endpoint_gain_error_exceeds(coeffs, kPi * 0.999f, gain_db, 1.5f)) {
    return rbj_high_shelf(w0, kButterworthQ, gain_db);
  }
  return coeffs;
}

BiquadCoeffs vicanek_low_shelf(float w0, float gain_db) {
  // Same source as high shelf, Appendix A.2. The low shelf is derived by designing the
  // reciprocal high shelf and scaling the numerator so the high-frequency gain is unity.
  const float gain = db_to_linear(gain_db);
  if (std::abs(gain - 1.0f) < 1.0e-6f) {
    return {};
  }
  const float fc = std::clamp(w0 / kPi, 1.0e-6f, 4.0f);
  const float g = 1.0f / gain;
  const float invg = 1.0f / g;
  const float fc2 = fc * fc;
  const float fc4 = fc2 * fc2;
  const float hny = (fc4 + g) / (fc4 + invg);

  const float f1 = fc / std::sqrt(0.160f + 1.543f * fc2);
  const float f14 = f1 * f1 * f1 * f1;
  const float h1 = (fc4 + f14 * g) / (fc4 + f14 * invg);
  const float phi1 = std::pow(std::sin(kHalfPi * f1), 2.0f);

  const float f2 = fc / std::sqrt(0.947f + 3.806f * fc2);
  const float f24 = f2 * f2 * f2 * f2;
  const float h2 = (fc4 + f24 * g) / (fc4 + f24 * invg);
  const float phi2 = std::pow(std::sin(kHalfPi * f2), 2.0f);

  const float d1 = (h1 - 1.0f) * (1.0f - phi1);
  const float c11 = -phi1 * d1;
  const float c12 = phi1 * phi1 * (hny - h1);
  const float d2 = (h2 - 1.0f) * (1.0f - phi2);
  const float c21 = -phi2 * d2;
  const float c22 = phi2 * phi2 * (hny - h2);
  const float denom = c11 * c22 - c12 * c21;
  const float alfa1 = safe_div(c22 * d1 - c12 * d2, denom, 0.0f);
  const float aa1 = safe_div(d1 - c11 * alfa1, c12, 1.0f);
  const float bb1 = hny * aa1;
  const float aa2 = 0.25f * (alfa1 - aa1);
  const float bb2 = 0.25f * (alfa1 - bb1);

  const float v = 0.5f * (1.0f + safe_sqrt(aa1));
  const float w = 0.5f * (1.0f + safe_sqrt(bb1));
  const float a0 = 0.5f * (v + safe_sqrt(v * v + aa2));
  const float inva0 = safe_div(1.0f, a0, 1.0f);
  const float ginva0 = invg * inva0;
  const float b0_base = 0.5f * (w + safe_sqrt(w * w + bb2));
  const BiquadCoeffs coeffs{b0_base * ginva0, (1.0f - w) * ginva0,
                            -0.25f * bb2 * ginva0 / std::max(b0_base, 1.0e-12f), (1.0f - v) * inva0,
                            -0.25f * aa2 * inva0 * inva0};
  if (endpoint_gain_error_exceeds(coeffs, 0.0f, gain_db, 1.5f)) {
    return rbj_low_shelf(w0, kButterworthQ, gain_db);
  }
  return coeffs;
}

float biquad_magnitude(const BiquadCoeffs& coeffs, float omega) {
  return magnitude_at(coeffs, omega);
}

float butterworth_stage_q(int order, int pair) {
  const double angle =
      (static_cast<double>(2 * pair + 1) * kPiD) / (2.0 * static_cast<double>(order));
  return static_cast<float>(1.0 / (2.0 * std::sin(angle)));
}

float one_pole_lowpass_alpha(float frequency_hz, double sample_rate) {
  const double g = 2.0 * kPiD * static_cast<double>(frequency_hz);
  return static_cast<float>(std::clamp(g / (g + sample_rate), 0.0, 1.0));
}

float one_pole_lowpass_alpha_matched(float frequency_hz, double sample_rate) {
  if (!(frequency_hz > 0.0f)) return 0.0f;
  if (!(sample_rate > 0.0)) return 1.0f;
  const float exponent =
      -sonare::constants::kTwoPi * frequency_hz / static_cast<float>(sample_rate);
  return std::clamp(1.0f - std::exp(exponent), 0.0f, 1.0f);
}

float one_pole_alpha_from_time_ms(float time_ms, double sample_rate) {
  if (!(sample_rate > 0.0)) return 1.0f;
  // 0.05 ms floor matches the legacy voice_changer::coeff_ms behavior so the
  // result saturates safely instead of underflowing to a divide-by-zero. The
  // coefficient is already > 0.999 by that point, so further clamping is
  // imperceptible.
  const float clamped = std::max(time_ms, 0.05f);
  const float exponent = -1.0f / (0.001f * clamped * static_cast<float>(sample_rate));
  return std::clamp(1.0f - std::exp(exponent), 0.0f, 1.0f);
}

float frequency_to_w0(float frequency_hz, double sample_rate) {
  if (!(sample_rate > 0.0)) return 0.0f;
  const float clamped = std::clamp(frequency_hz, 20.0f, static_cast<float>(sample_rate * 0.45));
  return sonare::constants::kTwoPi * clamped / static_cast<float>(sample_rate);
}

BiquadCoeffsD rbj_high_shelf_d(double frequency, double sample_rate, double gain_db, double q) {
  return rbj_high_shelf_from_design_d(rbj_high_shelf_design_d(frequency, sample_rate, q), gain_db);
}

HighShelfDesignD rbj_high_shelf_design_d(double frequency, double sample_rate, double q) {
  const double omega = 2.0 * kPiD * frequency / sample_rate;
  const double sin_omega = std::sin(omega);
  return {std::cos(omega), sin_omega / (2.0 * q)};
}

BiquadCoeffsD rbj_high_shelf_from_design_d(const HighShelfDesignD& design, double gain_db) {
  const double a = std::pow(10.0, gain_db / 40.0);
  const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * design.alpha;

  const double b0 = a * ((a + 1.0) + (a - 1.0) * design.cos_w0 + two_sqrt_a_alpha);
  const double b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * design.cos_w0);
  const double b2 = a * ((a + 1.0) + (a - 1.0) * design.cos_w0 - two_sqrt_a_alpha);
  const double a0 = (a + 1.0) - (a - 1.0) * design.cos_w0 + two_sqrt_a_alpha;
  const double a1 = 2.0 * ((a - 1.0) - (a + 1.0) * design.cos_w0);
  const double a2 = (a + 1.0) - (a - 1.0) * design.cos_w0 - two_sqrt_a_alpha;

  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoeffsD rbj_highpass_d(double frequency, double sample_rate, double q) {
  const double omega = 2.0 * kPiD * frequency / sample_rate;
  const double sin_omega = std::sin(omega);
  const double cos_omega = std::cos(omega);
  const double alpha = sin_omega / (2.0 * q);
  const double a0 = 1.0 + alpha;

  return {(1.0 + cos_omega) * 0.5 / a0, -(1.0 + cos_omega) / a0, (1.0 + cos_omega) * 0.5 / a0,
          -2.0 * cos_omega / a0, (1.0 - alpha) / a0};
}

BiquadCoeffsD rbj_bandpass_d(double frequency, double sample_rate, double q) {
  const auto raw = rbj_bandpass_raw_d(frequency, sample_rate, q);
  return {raw.b0 / raw.a0, raw.b1 / raw.a0, raw.b2 / raw.a0, raw.a1 / raw.a0, raw.a2 / raw.a0};
}

RawBiquadCoeffsD rbj_bandpass_raw_d(double frequency, double sample_rate, double q) {
  const double omega = 2.0 * kPiD * frequency / sample_rate;
  const double sin_omega = std::sin(omega);
  const double cos_omega = std::cos(omega);
  const double alpha = sin_omega / (2.0 * q);
  const double a0 = 1.0 + alpha;

  return {alpha, 0.0, -alpha, a0, -2.0 * cos_omega, 1.0 - alpha};
}

namespace {

BiquadCoeffsD deman_high_shelf_d(double frequency, double sample_rate, double gain_db, double q) {
  const double k = std::tan(kPiD * frequency / sample_rate);
  const double vh = std::pow(10.0, gain_db / 20.0);
  const double vb = std::pow(vh, 0.499666774155);
  const double a0 = 1.0 + k / q + k * k;
  return {
      (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,    (vh - vb * k / q + k * k) / a0,
      2.0 * (k * k - 1.0) / a0,       (1.0 - k / q + k * k) / a0,
  };
}

BiquadCoeffsD deman_highpass_d(double frequency, double sample_rate, double q) {
  const double k = std::tan(kPiD * frequency / sample_rate);
  const double a0 = 1.0 + k / q + k * k;
  return {
      1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0,
  };
}

}  // namespace

KWeightingCoeffs k_weighting_coefficients(double sample_rate) {
  // ITU-R BS.1770 reference coefficients are specified at 48 kHz; return them
  // verbatim to stay bit-exact with the standard, and derive other rates.
  if (sample_rate == 48000.0) {
    return {
        {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
         0.73248077421585},
        {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621},
    };
  }
  return {
      deman_high_shelf_d(1681.974450955533, sample_rate, 3.999843853973347, 0.7071752369554196),
      deman_highpass_d(38.13547087613982, sample_rate, 0.5003270373238773),
  };
}

}  // namespace sonare::rt
