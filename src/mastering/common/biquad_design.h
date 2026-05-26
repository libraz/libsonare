#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers for the mastering layer.

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::common {
using ::sonare::rt::biquad_magnitude;
using ::sonare::rt::BiquadCoeffs;
using ::sonare::rt::BiquadCoeffsD;
using ::sonare::rt::first_order_highpass;
using ::sonare::rt::first_order_lowpass;
using ::sonare::rt::k_weighting_coefficients;
using ::sonare::rt::KWeightingCoeffs;
using ::sonare::rt::rbj_bandpass;
using ::sonare::rt::rbj_high_shelf;
using ::sonare::rt::rbj_high_shelf_d;
using ::sonare::rt::rbj_highpass;
using ::sonare::rt::rbj_highpass_d;
using ::sonare::rt::rbj_low_shelf;
using ::sonare::rt::rbj_lowpass;
using ::sonare::rt::rbj_notch;
using ::sonare::rt::rbj_peak;
using ::sonare::rt::vicanek_bandpass;
using ::sonare::rt::vicanek_high_shelf;
using ::sonare::rt::vicanek_highpass;
using ::sonare::rt::vicanek_low_shelf;
using ::sonare::rt::vicanek_lowpass;
using ::sonare::rt::vicanek_notch;
using ::sonare::rt::vicanek_peak;

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

}  // namespace sonare::mastering::common
