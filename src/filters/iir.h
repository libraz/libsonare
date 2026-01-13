#pragma once

/// @file iir.h
/// @brief IIR filter (biquad) implementation.

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Biquad filter coefficients.
/// @details Transfer function: H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
struct BiquadCoeffs {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// @brief Creates highpass filter coefficients (Butterworth, 2nd order).
/// @param cutoff_hz Cutoff frequency in Hz
/// @param sr Sample rate in Hz
/// @return Biquad coefficients
BiquadCoeffs highpass_coeffs(float cutoff_hz, int sr);

/// @brief Creates lowpass filter coefficients (Butterworth, 2nd order).
/// @param cutoff_hz Cutoff frequency in Hz
/// @param sr Sample rate in Hz
/// @return Biquad coefficients
BiquadCoeffs lowpass_coeffs(float cutoff_hz, int sr);

/// @brief Creates bandpass filter coefficients (2nd order).
/// @param center_hz Center frequency in Hz
/// @param bandwidth_hz Bandwidth in Hz
/// @param sr Sample rate in Hz
/// @return Biquad coefficients
BiquadCoeffs bandpass_coeffs(float center_hz, float bandwidth_hz, int sr);

/// @brief Creates notch (band-reject) filter coefficients.
/// @param center_hz Center frequency in Hz
/// @param bandwidth_hz Bandwidth in Hz
/// @param sr Sample rate in Hz
/// @return Biquad coefficients
BiquadCoeffs notch_coeffs(float center_hz, float bandwidth_hz, int sr);

/// @brief Applies biquad filter to signal (in-place capable).
/// @param input Input signal
/// @param size Signal length
/// @param coeffs Biquad coefficients
/// @return Filtered signal
std::vector<float> apply_biquad(const float* input, size_t size, const BiquadCoeffs& coeffs);

/// @brief Applies biquad filter to signal.
/// @param input Input signal
/// @param coeffs Biquad coefficients
/// @return Filtered signal
std::vector<float> apply_biquad(const std::vector<float>& input, const BiquadCoeffs& coeffs);

/// @brief Applies biquad filter forward and backward (zero-phase filtering).
/// @param input Input signal
/// @param size Signal length
/// @param coeffs Biquad coefficients
/// @return Filtered signal (no phase distortion)
std::vector<float> apply_biquad_filtfilt(const float* input, size_t size,
                                         const BiquadCoeffs& coeffs);

}  // namespace sonare
