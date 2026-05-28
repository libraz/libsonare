#include "filters/iir.h"

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kTwoPi;
namespace {

BiquadCoeffs to_filter_coeffs(const rt::BiquadCoeffs& c) { return {c.b0, c.b1, c.b2, c.a1, c.a2}; }

float normalized_omega(float frequency_hz, int sr) {
  return constants::kTwoPi * frequency_hz / static_cast<float>(sr);
}

}  // namespace

BiquadCoeffs highpass_coeffs(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);
  return to_filter_coeffs(
      rt::rbj_highpass(normalized_omega(cutoff_hz, sr), constants::kButterworthQ));
}

BiquadCoeffs lowpass_coeffs(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);
  return to_filter_coeffs(
      rt::rbj_lowpass(normalized_omega(cutoff_hz, sr), constants::kButterworthQ));
}

BiquadCoeffs bandpass_coeffs(float center_hz, float bandwidth_hz, int sr) {
  SONARE_CHECK(center_hz > 0 && bandwidth_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(center_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  // Q = center / bandwidth
  const float q = center_hz / bandwidth_hz;
  return to_filter_coeffs(rt::rbj_bandpass(normalized_omega(center_hz, sr), q));
}

BiquadCoeffs notch_coeffs(float center_hz, float bandwidth_hz, int sr) {
  SONARE_CHECK(center_hz > 0 && bandwidth_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(center_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  const float q = center_hz / bandwidth_hz;
  return to_filter_coeffs(rt::rbj_notch(normalized_omega(center_hz, sr), q));
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

  CascadedBiquad cascade;
  cascade.sections.resize(2);
  const float w0 = normalized_omega(cutoff_hz, sr);
  cascade.sections[0] = to_filter_coeffs(rt::rbj_highpass(w0, kQ1));
  cascade.sections[1] = to_filter_coeffs(rt::rbj_highpass(w0, kQ2));

  return cascade;
}

CascadedBiquad lowpass_coeffs_4th(float cutoff_hz, int sr) {
  SONARE_CHECK(cutoff_hz > 0 && sr > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(cutoff_hz < sr / 2.0f, ErrorCode::InvalidParameter);

  constexpr float kQ1 = 0.5411961f;
  constexpr float kQ2 = 1.3065630f;

  CascadedBiquad cascade;
  cascade.sections.resize(2);
  const float w0 = normalized_omega(cutoff_hz, sr);
  cascade.sections[0] = to_filter_coeffs(rt::rbj_lowpass(w0, kQ1));
  cascade.sections[1] = to_filter_coeffs(rt::rbj_lowpass(w0, kQ2));

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
