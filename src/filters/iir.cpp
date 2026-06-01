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

/// @brief Direct Form II Transposed initial delay states (z1, z2).
struct BiquadZi {
  float z1 = 0.0f;
  float z2 = 0.0f;
};

/// @brief Computes the scipy.signal.lfilter_zi steady-state delay states for a
///        normalized biquad (a0 == 1), i.e. the DF2T state that yields a constant
///        output for a constant unit input. Multiplying the result by the boundary
///        sample value seeds each filtfilt pass at its edge steady state, which
///        removes the ~2*order start-up transient that zero initial conditions
///        introduce.
/// @details Solves (I - A) * zi = B with A = [[-a1, 1], [-a2, 0]] and
///          B = [b1 - a1*b0, b2 - a2*b0]. For a stable section
///          det = 1 + a1 + a2 != 0.
BiquadZi lfilter_zi(const BiquadCoeffs& c) {
  const float det = 1.0f + c.a1 + c.a2;
  if (std::abs(det) < constants::kEpsilon) {
    return {0.0f, 0.0f};
  }
  const float b0 = c.b1 - c.a1 * c.b0;
  const float b1 = c.b2 - c.a2 * c.b0;
  BiquadZi zi;
  zi.z1 = (b0 + b1) / det;
  zi.z2 = ((1.0f + c.a1) * b1 - c.a2 * b0) / det;
  return zi;
}

/// @brief Applies a DF2T biquad with explicit initial delay states.
std::vector<float> apply_biquad_zi(const float* input, size_t size, const BiquadCoeffs& coeffs,
                                   const BiquadZi& zi) {
  std::vector<float> output(size);
  float z1 = zi.z1;
  float z2 = zi.z2;
  for (size_t i = 0; i < size; ++i) {
    const float x = input[i];
    const float y = coeffs.b0 * x + z1;
    z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
    z2 = coeffs.b2 * x - coeffs.a2 * y;
    output[i] = y;
  }
  return output;
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

  // Seed each pass with scipy.signal.lfilter_zi-style steady-state initial
  // conditions scaled by the boundary sample, matching scipy.signal.filtfilt.
  // This suppresses the ~2*order edge transient that zero initial conditions
  // would otherwise inject at the start of each direction.
  const BiquadZi zi = lfilter_zi(coeffs);

  // Forward pass seeded at the leading edge.
  const BiquadZi zi_fwd{zi.z1 * input[0], zi.z2 * input[0]};
  std::vector<float> forward = apply_biquad_zi(input, size, coeffs, zi_fwd);

  // Reverse, then run the backward pass seeded at what is now the leading edge.
  std::reverse(forward.begin(), forward.end());
  const BiquadZi zi_bwd{zi.z1 * forward[0], zi.z2 * forward[0]};
  std::vector<float> backward = apply_biquad_zi(forward.data(), size, coeffs, zi_bwd);

  // Reverse again to restore the original time order.
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
