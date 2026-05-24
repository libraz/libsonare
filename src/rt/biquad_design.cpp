#include "rt/biquad_design.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

#include "util/constants.h"
#include "util/db.h"

namespace sonare::rt {

using sonare::constants::kHalfPi;
using sonare::constants::kPi;

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

float checked_q(float q) {
  if (!(q > 0.0f)) {
    throw std::invalid_argument("Q must be positive");
  }
  return std::max(q, 1.0e-6f);
}

float safe_sqrt(float x) { return std::sqrt(std::max(0.0f, x)); }

float safe_div(float numerator, float denominator, float fallback) {
  return std::abs(denominator) > 1.0e-12f ? numerator / denominator : fallback;
}

BiquadCoeffs normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
  if (!(std::abs(a0) > 0.0)) {
    throw std::runtime_error("invalid biquad coefficient normalization");
  }

  const double inv_a0 = 1.0 / a0;
  return {
      static_cast<float>(b0 * inv_a0), static_cast<float>(b1 * inv_a0),
      static_cast<float>(b2 * inv_a0), static_cast<float>(a1 * inv_a0),
      static_cast<float>(a2 * inv_a0),
  };
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
  return {b0, (1.0f - w) * inva0, -0.25f * bb2 * inva0 * inva0 / std::max(b0_unscaled, 1.0e-12f),
          (1.0f - v) * inva0, -0.25f * aa2 * inva0 * inva0};
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
  return {b0_base * ginva0, (1.0f - w) * ginva0,
          -0.25f * bb2 * ginva0 / std::max(b0_base, 1.0e-12f), (1.0f - v) * inva0,
          -0.25f * aa2 * inva0 * inva0};
}

float biquad_magnitude(const BiquadCoeffs& coeffs, float omega) {
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

}  // namespace sonare::rt
