#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers for the mastering layer.

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::common {
using namespace ::sonare::rt;

/// @brief Coefficients for a bilinear-transformed one-pole highpass.
///
/// The difference equation is y[n] = b0 * (x[n] - x[n-1]) + a1 * y[n-1], which
/// realizes a 6 dB/oct highpass whose cutoff is frequency-accurate (the bilinear
/// transform prewarps the analog cutoff). Shared by the dynamics-section
/// sidechain HPFs so all of them use the same formulation.
struct OnePoleHighpassCoeffs {
  float b0 = 1.0f;
  float a1 = 0.0f;
};

/// @brief Designs a one-pole highpass for @p cutoff_hz at @p sample_rate.
///
/// The cutoff is clamped to (1 Hz, 0.49 * sample_rate) so the design is stable
/// for any requested frequency. Math is performed in double precision.
inline OnePoleHighpassCoeffs onepole_highpass_coeffs(double cutoff_hz, double sample_rate) {
  const double clamped = std::clamp(cutoff_hz, 1.0, sample_rate * 0.49);
  const double g = std::tan(sonare::constants::kPiD * clamped / sample_rate);
  OnePoleHighpassCoeffs c;
  c.b0 = static_cast<float>(1.0 / (1.0 + g));
  c.a1 = static_cast<float>((1.0 - g) / (1.0 + g));
  return c;
}

/// @brief First-order (6 dB/oct) lowpass via bilinear transform.
/// @param w0 Normalized angular cutoff (2*pi*fc/fs).
inline BiquadCoeffs first_order_lowpass(float w0) {
  const double k = std::tan(static_cast<double>(w0) * 0.5);
  const double inv = 1.0 / (1.0 + k);
  const float b0 = static_cast<float>(k * inv);
  return {b0, b0, 0.0f, static_cast<float>((k - 1.0) * inv), 0.0f};
}

/// @brief First-order (6 dB/oct) highpass via bilinear transform.
/// @param w0 Normalized angular cutoff (2*pi*fc/fs).
inline BiquadCoeffs first_order_highpass(float w0) {
  const double k = std::tan(static_cast<double>(w0) * 0.5);
  const double inv = 1.0 / (1.0 + k);
  const float b0 = static_cast<float>(inv);
  return {b0, -b0, 0.0f, static_cast<float>((k - 1.0) * inv), 0.0f};
}

}  // namespace sonare::mastering::common
