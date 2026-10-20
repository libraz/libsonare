#pragma once

/// @file noise_shaping.h
/// @brief F-weighted error-feedback noise shaping, designed per sample rate.
/// @details The 9-tap filter of Lipshitz/Vanderkooy/Wannamaker ("Minimally
///          Audible Noise Shaping", JAES 1991) is published for 44.1 kHz. Its
///          noise transfer E(z) = 1 - sum c_k z^-(k+1) defines the target curve
///          in hertz, which keeps the noise minimum in the ear's most sensitive
///          1-5 kHz band; @ref design_noise_shaping refits the filter to that
///          curve at the rate the loop runs at.
///
///          The loop adds @ref noise_shaping_feedback and the dither to the
///          signal, quantizes, and pushes the error of the quantized value
///          against the signal plus feedback, dither excluded. The output is then
///          x - E(z) * error, so the dither is shaped along with the rounding.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "util/constants.h"

namespace sonare::mastering::final {

inline constexpr std::size_t kNoiseShapingTaps = 9;
/// Feedback coefficients c_k, in the loop's own sample order.
using NoiseShapingCoeffs = std::array<float, kNoiseShapingTaps>;
/// Past errors in quantizer steps, newest first.
using NoiseShapingHistory = std::array<float, kNoiseShapingTaps>;

namespace noise_shaping_detail {

inline constexpr std::array<double, kNoiseShapingTaps> kPublishedCoeffs = {
    2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847};
inline constexpr double kPublishedRate = 44100.0;
// Midpoint grid for the autocorrelation integral; 256 points agree with a
// 16384-point grid to 2e-5 in every coefficient up to 192 kHz, 2e-4 at 384 kHz.
inline constexpr int kDesignGrid = 256;

/// Target noise power |E(e^{j2 pi f / 44100})|^2, held at its 22.05 kHz value
/// above that so a faster rate may park noise in the band the curve never covered.
inline double published_noise_power(double hz) noexcept {
  const double omega =
      sonare::constants::kTwoPiD * std::min(hz, kPublishedRate / 2.0) / kPublishedRate;
  double re = 1.0;
  double im = 0.0;
  for (std::size_t k = 0; k < kNoiseShapingTaps; ++k) {
    const double phase = omega * static_cast<double>(k + 1);
    re -= kPublishedCoeffs[k] * std::cos(phase);
    im += kPublishedCoeffs[k] * std::sin(phase);
  }
  return re * re + im * im;
}

}  // namespace noise_shaping_detail

/// @brief Fits the published curve at @p sample_rate.
/// @details The order-9 linear predictor of a process whose spectrum is the
///          inverse target curve has, as its minimum-phase prediction-error
///          filter, the noise transfer closest to that curve. At 44.1 kHz that
///          process is exactly AR(9), so the published coefficients come back.
///          Costs a few thousand cosines; call it where the rate is set, not
///          per sample. Allocates nothing.
/// @param sample_rate Rate the error-feedback loop runs at, in Hz; positive.
inline NoiseShapingCoeffs design_noise_shaping(double sample_rate) noexcept {
  using noise_shaping_detail::kDesignGrid;
  const double nyquist = sample_rate / 2.0;
  std::array<double, kNoiseShapingTaps + 1> autocorrelation{};
  for (int i = 0; i < kDesignGrid; ++i) {
    const double hz = (static_cast<double>(i) + 0.5) * nyquist / kDesignGrid;
    const double weight = 1.0 / noise_shaping_detail::published_noise_power(hz);
    const double omega = sonare::constants::kTwoPiD * hz / sample_rate;
    for (std::size_t lag = 0; lag < autocorrelation.size(); ++lag) {
      autocorrelation[lag] += weight * std::cos(omega * static_cast<double>(lag));
    }
  }

  // Levinson-Durbin recursion for the monic prediction-error polynomial.
  std::array<double, kNoiseShapingTaps + 1> error_filter{};
  error_filter[0] = 1.0;
  double prediction_error = autocorrelation[0];
  for (std::size_t order = 1; order <= kNoiseShapingTaps; ++order) {
    double acc = autocorrelation[order];
    for (std::size_t j = 1; j < order; ++j) acc += error_filter[j] * autocorrelation[order - j];
    const double reflection = -acc / prediction_error;
    const auto previous = error_filter;
    for (std::size_t j = 1; j < order; ++j) {
      error_filter[j] = previous[j] + reflection * previous[order - j];
    }
    error_filter[order] = reflection;
    prediction_error *= 1.0 - reflection * reflection;
  }

  NoiseShapingCoeffs coeffs{};
  for (std::size_t k = 0; k < kNoiseShapingTaps; ++k) {
    coeffs[k] = static_cast<float>(-error_filter[k + 1]);
  }
  return coeffs;
}

/// @brief Feedback to add to the next input, in quantizer steps.
inline float noise_shaping_feedback(const NoiseShapingCoeffs& coeffs,
                                    const NoiseShapingHistory& history) noexcept {
  float feedback = 0.0f;
  for (std::size_t k = 0; k < kNoiseShapingTaps; ++k) feedback += coeffs[k] * history[k];
  return feedback;
}

/// @brief Records the error of one quantization, in quantizer steps.
/// @param error The value that was quantized, minus its dither, minus the
///        quantized result.
inline void push_noise_shaping_error(NoiseShapingHistory& history, float error) noexcept {
  for (std::size_t k = kNoiseShapingTaps - 1; k > 0; --k) history[k] = history[k - 1];
  history[0] = error;
}

}  // namespace sonare::mastering::final
