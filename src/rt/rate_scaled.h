#pragma once

/// @file rate_scaled.h
/// @brief Coefficients and sizes built from physical quantities (Hz, seconds) at the prepared
///        sample rate, so one configuration measures the same at every rate.
///
/// A coefficient is a function of the sample rate; a value measured at one rate is held as hertz
/// or seconds and turned into a coefficient, a clamp or a buffer length here, where the rate is
/// known. Header-only and allocation-free.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::rt {

/// The sample rate shipped coefficients and floors were voiced at.
constexpr double kVoicedSampleRate = 48000.0;

/// Re-expresses a loss one-pole coefficient `y += alpha * (x - y)` voiced at kVoicedSampleRate so
/// it keeps its corner in Hz at @p sample_rate: `1 - (1 - alpha)^(48000 / sr)`. Computed in double
/// so the voiced rate returns @p alpha_voiced bit for bit.
inline float loss_alpha_at_rate(float alpha_voiced, double sample_rate) noexcept {
  if (!(sample_rate > 0.0) || !(alpha_voiced < 1.0f)) return alpha_voiced;
  const double a = 1.0 - static_cast<double>(alpha_voiced);
  return static_cast<float>(1.0 - std::pow(a, kVoicedSampleRate / sample_rate));
}

/// one_pole_lowpass_alpha_matched() floored at the 0.01 coefficient of the voiced rate re-expressed
/// at @p sample_rate, so the floor is a corner of about 76 Hz at every rate.
inline float lowpass_alpha_at_rate(float corner_hz, double sample_rate) noexcept {
  return std::max(one_pole_lowpass_alpha_matched(corner_hz, sample_rate),
                  loss_alpha_at_rate(0.01f, sample_rate));
}

/// Fit band of voiced_one_pole_at_rate(): the lowest point sits where a per-pass loss already
/// departs from DC, the highest at the top of hearing.
constexpr double kVoicedFitLowestHz = 100.0;
constexpr double kVoicedFitHighestHz = 20000.0;
constexpr int kVoicedFitPoints = 24;
constexpr int kVoicedFitIterations = 8;
/// Keeps the fitted pole and zero strictly inside the unit circle.
constexpr double kVoicedFitMaxRoot = 0.999;

/// Coefficients of `y = b0 x + b1 x1 + a1 y1`.
struct FirstOrderShelf {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float a1 = 0.0f;
};

/// Re-expresses the one-pole `y = b0 x + a1 y1` voiced at kVoicedSampleRate at @p sample_rate as a
/// first-order section with the same loss in Hz. DC is matched exactly; the pole and zero are a
/// least-squares fit of the relative per-pass loss in dB over log-spaced points from
/// kVoicedFitLowestHz to min(Nyquist, kVoicedFitHighestHz), started from a three-point match.
/// A relative loss error is the relative T60 error of a band, which is what compounds over many
/// passes. The voiced rate returns {b0, 0, a1} unchanged, so it keeps its output bit for bit.
inline FirstOrderShelf voiced_one_pole_at_rate(float b0, float a1, double sample_rate) noexcept {
  const FirstOrderShelf voiced{b0, 0.0f, a1};
  if (sample_rate == kVoicedSampleRate || a1 == 0.0f || !(sample_rate > 0.0)) return voiced;
  const double vb = b0;
  const double va = a1;
  const double voiced_rad_per_hz = constants::kTwoPiD / kVoicedSampleRate;
  const auto voiced_power = [&](double hz) {
    return vb * vb / (1.0 - 2.0 * va * std::cos(voiced_rad_per_hz * hz) + va * va);
  };
  const double top_hz = std::min(0.5 * sample_rate, 0.5 * kVoicedSampleRate);
  const double dc = voiced_power(0.0);
  const double top = voiced_power(top_hz);
  const double mid = std::sqrt(dc * top);
  const double cos_mid_voiced = std::clamp((1.0 + va * va - vb * vb / mid) / (2.0 * va), -1.0, 1.0);
  const double mid_hz = std::acos(cos_mid_voiced) / voiced_rad_per_hz;
  // |H|^2 = (n0 - n1 x) / (1 - d1 x) in x = cos(w): three matched points are a 3x3 linear solve.
  const double rad_per_hz = constants::kTwoPiD / sample_rate;
  const double x[3] = {1.0, std::cos(rad_per_hz * mid_hz), std::cos(rad_per_hz * top_hz)};
  const double t[3] = {dc, mid, top};
  // Rows (1, -x, t x) . (n0, n1, d1) = t.
  const auto det3 = [](const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  double a[3][3];
  for (int i = 0; i < 3; ++i) {
    a[i][0] = 1.0;
    a[i][1] = -x[i];
    a[i][2] = t[i] * x[i];
  }
  const double det = det3(a);
  if (!(std::abs(det) > 0.0)) return voiced;
  double solved[3];
  for (int c = 0; c < 3; ++c) {
    double m[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) m[i][j] = j == c ? t[i] : a[i][j];
    }
    solved[c] = det3(m) / det;
  }
  const double n0 = solved[0];
  const double n1 = solved[1];
  const double d1 = solved[2];
  // 2 r / (1 + r^2) = q has the root r = q / (1 + sqrt(1 - q^2)) inside the unit circle.
  const auto root = [](double q) { return q / (1.0 + std::sqrt(1.0 - q * q)); };
  if (!(n0 > 0.0) || !(std::abs(d1) < 1.0) || !(std::abs(n1 / n0) < 1.0)) return voiced;
  double pole = root(d1);
  double zero = root(n1 / n0);

  // Gauss-Newton on r_i = (L_i - loss_i) / loss_i, L the fitted loss in nepers with DC pinned.
  std::array<double, kVoicedFitPoints> cos_w{};
  std::array<double, kVoicedFitPoints> loss{};
  const double high_hz = std::min(0.5 * sample_rate, kVoicedFitHighestHz);
  for (int i = 0; i < kVoicedFitPoints; ++i) {
    const double hz =
        kVoicedFitLowestHz *
        std::pow(high_hz / kVoicedFitLowestHz, static_cast<double>(i) / (kVoicedFitPoints - 1));
    cos_w[static_cast<size_t>(i)] = std::cos(rad_per_hz * hz);
    loss[static_cast<size_t>(i)] = -std::log(voiced_power(hz));
    if (!(loss[static_cast<size_t>(i)] > 0.0)) return voiced;
  }
  const double dc_loss = -std::log(dc);
  const auto fitted_loss = [&](double z, double p, double c) {
    return dc_loss + 2.0 * std::log((1.0 - z) / (1.0 - p)) - std::log(1.0 - 2.0 * z * c + z * z) +
           std::log(1.0 - 2.0 * p * c + p * p);
  };
  for (int iteration = 0; iteration < kVoicedFitIterations; ++iteration) {
    double jzz = 0.0;
    double jzp = 0.0;
    double jpp = 0.0;
    double gz = 0.0;
    double gp = 0.0;
    for (size_t i = 0; i < cos_w.size(); ++i) {
      const double c = cos_w[i];
      const double r = (fitted_loss(zero, pole, c) - loss[i]) / loss[i];
      const double dz =
          (-2.0 / (1.0 - zero) - (2.0 * zero - 2.0 * c) / (1.0 - 2.0 * zero * c + zero * zero)) /
          loss[i];
      const double dp =
          (2.0 / (1.0 - pole) + (2.0 * pole - 2.0 * c) / (1.0 - 2.0 * pole * c + pole * pole)) /
          loss[i];
      jzz += dz * dz;
      jzp += dz * dp;
      jpp += dp * dp;
      gz += dz * r;
      gp += dp * r;
    }
    const double normal_det = jzz * jpp - jzp * jzp;
    if (!(std::abs(normal_det) > 0.0)) break;
    zero = std::clamp(zero - (jpp * gz - jzp * gp) / normal_det, -kVoicedFitMaxRoot,
                      kVoicedFitMaxRoot);
    pole = std::clamp(pole - (jzz * gp - jzp * gz) / normal_det, -kVoicedFitMaxRoot,
                      kVoicedFitMaxRoot);
  }
  const double gain = std::sqrt(dc) * (1.0 - pole) / (1.0 - zero);
  if (!std::isfinite(gain)) return voiced;
  return {static_cast<float>(gain), static_cast<float>(-gain * zero), static_cast<float>(pole)};
}

/// Samples spanning @p seconds at @p sample_rate, rounded up (at least @p minimum).
inline size_t samples_for_seconds(double seconds, double sample_rate, size_t minimum = 1) noexcept {
  if (!(seconds > 0.0) || !(sample_rate > 0.0)) return minimum;
  return std::max(minimum, static_cast<size_t>(std::ceil(seconds * sample_rate)));
}

/// Per-pass gain that decays by 60 dB in @p t60_s when a pass lasts @p pass_seconds.
inline float pass_gain_for_t60(float pass_seconds, float t60_s) noexcept {
  return std::pow(10.0f, -3.0f * pass_seconds / std::max(1.0e-6f, t60_s));
}

}  // namespace sonare::rt
