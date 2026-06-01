#pragma once

/// @file biquad_design.h
/// @brief Biquad coefficient design helpers.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::rt {

inline constexpr double kLoudnessOffset = -0.691;

struct BiquadCoeffs {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// @brief Transposed Direct Form II biquad runtime state.
/// @details Shared by realtime processors that need a small per-stage filter
///          (voice changer, formant warp, etc.) instead of re-declaring an
///          identical state struct. Coefficients are owned externally so callers
///          can update them between blocks (RT-safe scalar math only).
struct BiquadState {
  BiquadCoeffs c;
  float z1 = 0.0f;
  float z2 = 0.0f;

  void set(BiquadCoeffs coeffs) noexcept { c = coeffs; }
  void reset() noexcept {
    z1 = 0.0f;
    z2 = 0.0f;
  }
  float process(float x) noexcept {
    const float y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
  }
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

/// @brief Matched-z one-pole low-pass coefficient `1 - exp(-2*pi*fc/fs)`.
/// @details Returns a clamped coefficient in `[0, 1]`.
float one_pole_lowpass_alpha_matched(float frequency_hz, double sample_rate);

/// @brief Time-constant parameterization of @ref one_pole_lowpass_alpha_matched.
/// @details Returns `1 - exp(-1 / (tau * sample_rate))` where
///          `tau = max(0.05, time_ms) * 0.001`. The 0.05 ms floor guards
///          against denormals and divide-by-zero; the coefficient saturates
///          near 1.0 below that anyway, so the clamp is audibly harmless.
///          Equivalent in shape to the frequency-domain version with
///          `frequency_hz = 1 / (2*pi*tau)`, but expressed in milliseconds for
///          envelope followers / gates / compressors / limiters whose A/R
///          settings are naturally in time units.
/// @return Coefficient in `[0, 1]`.
float one_pole_alpha_from_time_ms(float time_ms, double sample_rate);

/// @brief Normalized angular frequency `2*pi*frequency_hz/sample_rate`.
/// @details The frequency is clamped to `[20 Hz, sample_rate*0.45]` so biquad
///          designs stay safely inside the unit circle and away from Nyquist.
///          The RBJ/Vicanek designers in this header all expect this `w0` form.
float frequency_to_w0(float frequency_hz, double sample_rate);

/// @brief Coefficients for a bilinear-transformed one-pole highpass.
///
/// The difference equation is y[n] = b0 * (x[n] - x[n-1]) + a1 * y[n-1], which
/// realizes a 6 dB/oct highpass whose cutoff is frequency-accurate.
struct OnePoleHighpassCoeffs {
  float b0 = 1.0f;
  float a1 = 0.0f;
};

/// @brief Designs a one-pole highpass for @p cutoff_hz at @p sample_rate.
/// @details Math is performed in double precision and the cutoff is clamped to
/// a stable audio range.
inline OnePoleHighpassCoeffs onepole_highpass_coeffs(double cutoff_hz, double sample_rate) {
  const double clamped = std::clamp(cutoff_hz, 1.0, sample_rate * 0.49);
  const double g = std::tan(sonare::constants::kPiD * clamped / sample_rate);
  OnePoleHighpassCoeffs c;
  c.b0 = static_cast<float>(1.0 / (1.0 + g));
  c.a1 = static_cast<float>((1.0 - g) / (1.0 + g));
  return c;
}

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

/// @brief Transposed Direct Form II biquad runtime state (double precision).
/// @details Double-precision counterpart of @ref BiquadState, used by filters
///          that need accumulation precision (K-weighting, dynamic-EQ
///          detectors). Coefficients are owned externally (in @c c) so callers
///          can update them between blocks (RT-safe scalar math only).
struct BiquadStateD {
  BiquadCoeffsD c;
  double z1 = 0.0;
  double z2 = 0.0;

  void set(BiquadCoeffsD coeffs) noexcept { c = coeffs; }
  void reset() noexcept {
    z1 = 0.0;
    z2 = 0.0;
  }
  double process(double x) noexcept {
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
  }
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
