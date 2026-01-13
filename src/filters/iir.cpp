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

}  // namespace sonare
