#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers for the mastering layer.

#include "rt/biquad_design.h"

namespace sonare::mastering::common {
using ::sonare::rt::biquad_magnitude;
using ::sonare::rt::BiquadCoeffs;
using ::sonare::rt::BiquadCoeffsD;
using ::sonare::rt::first_order_highpass;
using ::sonare::rt::first_order_lowpass;
using ::sonare::rt::k_weighting_coefficients;
using ::sonare::rt::KWeightingCoeffs;
using ::sonare::rt::onepole_highpass_coeffs;
using ::sonare::rt::OnePoleHighpassCoeffs;
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

}  // namespace sonare::mastering::common
