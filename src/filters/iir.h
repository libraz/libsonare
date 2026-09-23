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

/// @brief Cascaded biquad sections for higher-order filters.
struct CascadedBiquad {
  std::vector<BiquadCoeffs> sections;
};

/// @brief Creates 4th order Butterworth highpass filter coefficients.
/// @param cutoff_hz Cutoff frequency in Hz
/// @param sr Sample rate in Hz
/// @return Cascaded biquad sections (2 sections for 4th order)
CascadedBiquad highpass_coeffs_4th(float cutoff_hz, int sr);

/// @brief Creates 4th order Butterworth lowpass filter coefficients.
/// @param cutoff_hz Cutoff frequency in Hz
/// @param sr Sample rate in Hz
/// @return Cascaded biquad sections (2 sections for 4th order)
CascadedBiquad lowpass_coeffs_4th(float cutoff_hz, int sr);

/// @brief Applies cascaded biquad sections in the forward (causal) direction.
/// @param input Input signal
/// @param size Signal length
/// @param cascade Cascaded biquad sections
/// @return Filtered signal with the cascade's causal phase response
std::vector<float> apply_cascade(const float* input, size_t size, const CascadedBiquad& cascade);

/// @brief Applies cascaded biquad filter forward and backward (zero-phase filtering).
/// @param input Input signal
/// @param size Signal length
/// @param cascade Cascaded biquad sections
/// @return Filtered signal (no phase distortion)
std::vector<float> apply_cascade_filtfilt(const float* input, size_t size,
                                          const CascadedBiquad& cascade);

/// @brief Zero-phase Butterworth lowpass or highpass of even @p order, applied in place.
/// @details A forward then a backward pass over order/2 second-order sections, with double
///          precision state and zero initial conditions (the signal is taken as zero outside the
///          buffer). The response is |H|^2 of the order-@p order Butterworth: -6 dB at the
///          cutoff and 12 * @p order dB/oct beyond it, so a lowpass and a highpass at the same
///          cutoff are magnitude-complementary and sum to the input.
/// @param x Signal, filtered in place
/// @param cutoff_hz Cutoff frequency in Hz, below Nyquist
/// @param sr Sample rate in Hz
/// @param order Even Butterworth order (2, 4, 6, ...)
/// @param highpass True for a highpass, false for a lowpass
void butterworth_zero_phase(std::vector<float>& x, float cutoff_hz, int sr, int order,
                            bool highpass);

/// @brief Applies a zero-initial-state pre-emphasis filter to audio signal.
/// @details Internal legacy helper. Unlike effects/preemphasis.h this starts
/// at zero state, so its distinct name prevents accidental overload selection.
/// @param input Input signal
/// @param size Signal length
/// @param coeff Pre-emphasis coefficient (default 0.97)
/// @return Filtered signal
std::vector<float> preemphasis_zero_initial_state(const float* input, size_t size,
                                                  float coeff = 0.97f);

}  // namespace sonare
