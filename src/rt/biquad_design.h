#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers.

namespace sonare::rt {

inline constexpr double kLoudnessOffset = -0.691;

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

/// @brief First-order (6 dB/oct) lowpass via bilinear transform.
/// @param w0 Normalized angular cutoff (2*pi*fc/fs).
BiquadCoeffs first_order_lowpass(float w0);

/// @brief First-order (6 dB/oct) highpass via bilinear transform.
/// @param w0 Normalized angular cutoff (2*pi*fc/fs).
BiquadCoeffs first_order_highpass(float w0);

/// @brief Evaluate |H(e^jw)| for normalized biquad coefficients.
float biquad_magnitude(const BiquadCoeffs& coeffs, float omega);

/// @brief Q value for one second-order Butterworth section in an @p order cascade.
float butterworth_stage_q(int order, int pair);

/// @brief One-pole low-pass smoothing coefficient `g / (g + sample_rate)`.
/// @details `g = 2*pi*frequency_hz`; result is clamped to `[0, 1]`.
float one_pole_lowpass_alpha(float frequency_hz, double sample_rate);

/// @brief Normalized second-order section with double-precision coefficients.
/// @details Used by the ITU-R BS.1770 K-weighting filters where the accumulation
/// precision matters (long integration windows). Direct Form II transposed.
struct BiquadCoeffsD {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
};

struct RawBiquadCoeffsD {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a0 = 1.0;
  double a1 = 0.0;
  double a2 = 0.0;
};

struct HighShelfDesignD {
  double cos_w0 = 1.0;
  double alpha = 0.0;
};

/// @brief RBJ high-shelf design in double precision (normalized by a0).
BiquadCoeffsD rbj_high_shelf_d(double frequency, double sample_rate, double gain_db, double q);

/// @brief Precompute gain-independent terms for a double-precision RBJ high-shelf.
HighShelfDesignD rbj_high_shelf_design_d(double frequency, double sample_rate, double q);

/// @brief Apply gain to a precomputed double-precision RBJ high-shelf design.
BiquadCoeffsD rbj_high_shelf_from_design_d(const HighShelfDesignD& design, double gain_db);

/// @brief RBJ high-pass design in double precision (normalized by a0).
BiquadCoeffsD rbj_highpass_d(double frequency, double sample_rate, double q);

/// @brief RBJ constant-skirt-gain band-pass design in double precision (normalized by a0).
BiquadCoeffsD rbj_bandpass_d(double frequency, double sample_rate, double q);

/// @brief RBJ constant-skirt-gain band-pass design before a0 normalization.
RawBiquadCoeffsD rbj_bandpass_raw_d(double frequency, double sample_rate, double q);

/// @brief The two ITU-R BS.1770 K-weighting biquads (pre-filter shelf + RLB high-pass).
struct KWeightingCoeffs {
  BiquadCoeffsD pre;  ///< Stage 1: high-shelf pre-filter.
  BiquadCoeffsD rlb;  ///< Stage 2: RLB high-pass.
};

/// @brief Designs the ITU-R BS.1770 K-weighting coefficients for @p sample_rate.
/// @details Returns the exact reference coefficients for 48000 Hz, otherwise
/// derives the shelf + high-pass via the analytic BS.1770 formulas.
KWeightingCoeffs k_weighting_coefficients(double sample_rate);

}  // namespace sonare::rt
