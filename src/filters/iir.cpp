#include "filters/iir.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

BiquadCoeffs highpass_coeffs(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  // Butterworth highpass (2nd order)
  float omega = 2.0f * kPi * cutoff_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);
  float alpha = sin_omega / std::sqrt(2.0f);  // Q = 1/sqrt(2) for Butterworth

  float a0 = 1.0f + alpha;

  BiquadCoeffs coeffs;
  coeffs.b0 = (1.0f + cos_omega) / 2.0f / a0;
  coeffs.b1 = -(1.0f + cos_omega) / a0;
  coeffs.b2 = (1.0f + cos_omega) / 2.0f / a0;
  coeffs.a1 = -2.0f * cos_omega / a0;
  coeffs.a2 = (1.0f - alpha) / a0;

  return coeffs;
}

BiquadCoeffs lowpass_coeffs(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  // Butterworth lowpass (2nd order)
  float omega = 2.0f * kPi * cutoff_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);
  float alpha = sin_omega / std::sqrt(2.0f);  // Q = 1/sqrt(2) for Butterworth

  float a0 = 1.0f + alpha;

  BiquadCoeffs coeffs;
  coeffs.b0 = (1.0f - cos_omega) / 2.0f / a0;
  coeffs.b1 = (1.0f - cos_omega) / a0;
  coeffs.b2 = (1.0f - cos_omega) / 2.0f / a0;
  coeffs.a1 = -2.0f * cos_omega / a0;
  coeffs.a2 = (1.0f - alpha) / a0;

  return coeffs;
}

BiquadCoeffs bandpass_coeffs(float center_hz, float bandwidth_hz, int sr) {
  SONARE_CHECK(center_hz > 0 && bandwidth_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(center_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  float omega = 2.0f * kPi * center_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);

  // Q = center / bandwidth
  float Q = center_hz / bandwidth_hz;
  float alpha = sin_omega / (2.0f * Q);

  float a0 = 1.0f + alpha;

  BiquadCoeffs coeffs;
  coeffs.b0 = alpha / a0;
  coeffs.b1 = 0.0f;
  coeffs.b2 = -alpha / a0;
  coeffs.a1 = -2.0f * cos_omega / a0;
  coeffs.a2 = (1.0f - alpha) / a0;

  return coeffs;
}

BiquadCoeffs notch_coeffs(float center_hz, float bandwidth_hz, int sr) {
  SONARE_CHECK(center_hz > 0 && bandwidth_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(center_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  float omega = 2.0f * kPi * center_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);

  float Q = center_hz / bandwidth_hz;
  float alpha = sin_omega / (2.0f * Q);

  float a0 = 1.0f + alpha;

  BiquadCoeffs coeffs;
  coeffs.b0 = 1.0f / a0;
  coeffs.b1 = -2.0f * cos_omega / a0;
  coeffs.b2 = 1.0f / a0;
  coeffs.a1 = -2.0f * cos_omega / a0;
  coeffs.a2 = (1.0f - alpha) / a0;

  return coeffs;
}

std::vector<float> apply_biquad(const float* input, size_t size, const BiquadCoeffs& coeffs) {
  if (size == 0) {
    return {};
  }
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size);

  // Direct Form II Transposed
  float z1 = 0.0f;
  float z2 = 0.0f;

  for (size_t i = 0; i < size; ++i) {
    float x = input[i];
    float y = coeffs.b0 * x + z1;
    z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
    z2 = coeffs.b2 * x - coeffs.a2 * y;
    output[i] = y;
  }

  return output;
}

std::vector<float> apply_biquad(const std::vector<float>& input, const BiquadCoeffs& coeffs) {
  return apply_biquad(input.data(), input.size(), coeffs);
}

std::vector<float> apply_biquad_filtfilt(const float* input, size_t size,
                                         const BiquadCoeffs& coeffs) {
  SONARE_CHECK(input != nullptr && size > 0, ErrorCode::InvalidParameter);

  // Forward pass
  std::vector<float> forward = apply_biquad(input, size, coeffs);

  // Reverse
  std::reverse(forward.begin(), forward.end());

  // Backward pass
  std::vector<float> backward = apply_biquad(forward.data(), size, coeffs);

  // Reverse again
  std::reverse(backward.begin(), backward.end());

  return backward;
}

CascadedBiquad highpass_coeffs_4th(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  // 4th order Butterworth = cascade of two 2nd order sections
  // Each section uses different Q values for proper Butterworth response
  // Q1 = 1/(2*cos(pi/8)) ≈ 0.541
  // Q2 = 1/(2*cos(3*pi/8)) ≈ 1.307
  constexpr float kQ1 = 0.5411961f;  // 1/(2*cos(pi/8))
  constexpr float kQ2 = 1.3065630f;  // 1/(2*cos(3*pi/8))

  float omega = 2.0f * kPi * cutoff_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);

  CascadedBiquad cascade;
  cascade.sections.resize(2);

  // Section 1 (Q = Q1)
  {
    float alpha = sin_omega / (2.0f * kQ1);
    float a0 = 1.0f + alpha;
    BiquadCoeffs& c = cascade.sections[0];
    c.b0 = (1.0f + cos_omega) / 2.0f / a0;
    c.b1 = -(1.0f + cos_omega) / a0;
    c.b2 = (1.0f + cos_omega) / 2.0f / a0;
    c.a1 = -2.0f * cos_omega / a0;
    c.a2 = (1.0f - alpha) / a0;
  }

  // Section 2 (Q = Q2)
  {
    float alpha = sin_omega / (2.0f * kQ2);
    float a0 = 1.0f + alpha;
    BiquadCoeffs& c = cascade.sections[1];
    c.b0 = (1.0f + cos_omega) / 2.0f / a0;
    c.b1 = -(1.0f + cos_omega) / a0;
    c.b2 = (1.0f + cos_omega) / 2.0f / a0;
    c.a1 = -2.0f * cos_omega / a0;
    c.a2 = (1.0f - alpha) / a0;
  }

  return cascade;
}

CascadedBiquad lowpass_coeffs_4th(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  constexpr float kQ1 = 0.5411961f;
  constexpr float kQ2 = 1.3065630f;

  float omega = 2.0f * kPi * cutoff_hz / sr;
  float cos_omega = std::cos(omega);
  float sin_omega = std::sin(omega);

  CascadedBiquad cascade;
  cascade.sections.resize(2);

  // Section 1 (Q = Q1)
  {
    float alpha = sin_omega / (2.0f * kQ1);
    float a0 = 1.0f + alpha;
    BiquadCoeffs& c = cascade.sections[0];
    c.b0 = (1.0f - cos_omega) / 2.0f / a0;
    c.b1 = (1.0f - cos_omega) / a0;
    c.b2 = (1.0f - cos_omega) / 2.0f / a0;
    c.a1 = -2.0f * cos_omega / a0;
    c.a2 = (1.0f - alpha) / a0;
  }

  // Section 2 (Q = Q2)
  {
    float alpha = sin_omega / (2.0f * kQ2);
    float a0 = 1.0f + alpha;
    BiquadCoeffs& c = cascade.sections[1];
    c.b0 = (1.0f - cos_omega) / 2.0f / a0;
    c.b1 = (1.0f - cos_omega) / a0;
    c.b2 = (1.0f - cos_omega) / 2.0f / a0;
    c.a1 = -2.0f * cos_omega / a0;
    c.a2 = (1.0f - alpha) / a0;
  }

  return cascade;
}

std::vector<float> apply_cascade_filtfilt(const float* input, size_t size,
                                          const CascadedBiquad& cascade) {
  SONARE_CHECK(input != nullptr && size > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(!cascade.sections.empty(), ErrorCode::InvalidParameter);

  // Apply each section's filtfilt in sequence
  std::vector<float> result(input, input + size);

  for (const auto& section : cascade.sections) {
    result = apply_biquad_filtfilt(result.data(), result.size(), section);
  }

  return result;
}

std::vector<float> preemphasis(const float* input, size_t size, float coeff) {
  if (size == 0) {
    return {};
  }
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size);
  output[0] = input[0];

  for (size_t i = 1; i < size; ++i) {
    output[i] = input[i] - coeff * input[i - 1];
  }

  return output;
}

}  // namespace sonare
