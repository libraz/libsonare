#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers.

namespace sonare::mastering::common {

struct BiquadCoeffs {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

BiquadCoeffs vicanek_lowpass(float w0, float q);
BiquadCoeffs vicanek_highpass(float w0, float q);
BiquadCoeffs vicanek_bandpass(float w0, float q);
BiquadCoeffs vicanek_notch(float w0, float q);
BiquadCoeffs vicanek_peak(float w0, float q, float gain_db);
BiquadCoeffs vicanek_high_shelf(float w0, float gain_db);
BiquadCoeffs vicanek_low_shelf(float w0, float gain_db);

BiquadCoeffs rbj_lowpass(float w0, float q);
BiquadCoeffs rbj_highpass(float w0, float q);
BiquadCoeffs rbj_bandpass(float w0, float q);
BiquadCoeffs rbj_notch(float w0, float q);
BiquadCoeffs rbj_peak(float w0, float q, float gain_db);
BiquadCoeffs rbj_high_shelf(float w0, float q, float gain_db);
BiquadCoeffs rbj_low_shelf(float w0, float q, float gain_db);

}  // namespace sonare::mastering::common
