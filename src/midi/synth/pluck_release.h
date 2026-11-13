#pragma once

/// @file pluck_release.h
/// @brief Finite-duration pluck excitation shared by the Karplus-Strong and plucked-string cores.
///
/// The burst is the string held in a triangle and let go over a release time: a parabola whose
/// series is n^-2 (the held shape), combed at the plucking point, then smoothed by a raised-cosine
/// release kernel whose spectrum is contact_spectrum(f * tau). Mode n therefore carries
/// |sin(n pi beta)| n^-2 W(n f0 tau). Pure functions over the burst geometry; no state.

#include <algorithm>
#include <cmath>

#include "midi/synth/contact_force.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// The softest velocity the release is shortened from, so a velocity of 0 cannot drive
/// vel01 ^ -pluck_vel_exp to infinity.
inline constexpr float kPluckMinVelocity = 1.0f / 127.0f;
/// The release kernel is at least this many samples long: two is a one-sample delay.
inline constexpr int kPluckMinSpan = 2;

/// The geometry one pluck burst is built from.
struct PluckBurst {
  int total;       ///< Samples of the held shape (the burst's floor length).
  float period;    ///< The string's period in samples, which the parabola spans.
  int pick_delay;  ///< Comb delay in samples (0 = no comb).
  int span;        ///< Release kernel length in samples (>= kPluckMinSpan).
};

/// Release kernel length in samples for a pluck of @p pluck_ms, shortened as the stroke hardens:
/// tau = pluck_ms * vel01 ^ -pluck_vel_exp.
inline int pluck_release_samples(float pluck_ms, float pluck_vel_exp, float vel01,
                                 double sample_rate) noexcept {
  const float tau_ms =
      pluck_ms * std::pow(std::max(vel01, kPluckMinVelocity), -std::max(0.0f, pluck_vel_exp));
  return std::max(kPluckMinSpan, contact_pulse_samples(tau_ms, sample_rate) - 1);
}

/// Samples the burst runs for: the held shape and its comb copy, plus the release kernel's tail.
inline int pluck_injection_length(const PluckBurst& b) noexcept {
  return b.total + b.pick_delay + (b.span > 0 ? b.span - 1 : 0);
}

/// Sample @p k of the string-in-a-triangle source: the second Bernoulli polynomial
/// u^2 - u + 1/6 over u = k / period, zero-mean, with the series sum cos(2 pi n u) / (pi^2 n^2).
/// Scaled by pi^2 so mode n of the burst is exactly n^-2. The first and last sample close the cell
/// the fractional period leaves, as a trapezoid does.
inline float pluck_parabola(const PluckBurst& b, int k) noexcept {
  const float u = static_cast<float>(k) / b.period;
  const float edge =
      k == 0 || k == b.total - 1 ? 1.0f + 0.5f * (b.period - static_cast<float>(b.total)) : 1.0f;
  return edge * constants::kPi * constants::kPi * (u * u - u + 1.0f / 6.0f);
}

/// The burst at position @p pos: the comb source[pos] - source[pos - pick_delay] makes the
/// parabola a triangle with its apex at the plucking point, then the raised-cosine kernel
/// h[j] = (1 - cos(2 pi j / span)) / span (taps summing to one) smooths it. Unscaled: the caller
/// applies its own comb headroom.
inline float pluck_release(const PluckBurst& b, int pos) noexcept {
  const int span = b.span;
  const int last = b.total + b.pick_delay - 1;
  const int j_lo = std::max(0, pos - last);
  const int j_hi = std::min(span - 1, pos);
  if (j_lo > j_hi) return 0.0f;
  const double step = constants::kTwoPiD / static_cast<double>(span);
  const double twice_cos = 2.0 * std::cos(step);
  double c_prev = std::cos(step * (j_lo - 1));
  double c = std::cos(step * j_lo);
  double acc = 0.0;
  for (int j = j_lo; j <= j_hi; ++j) {
    const int k = pos - j;
    float source = k < b.total ? pluck_parabola(b, k) : 0.0f;
    if (b.pick_delay > 0 && k >= b.pick_delay) source -= pluck_parabola(b, k - b.pick_delay);
    acc += (1.0 - c) * static_cast<double>(source);
    const double c_next = twice_cos * c - c_prev;
    c_prev = c;
    c = c_next;
  }
  return static_cast<float>(acc / static_cast<double>(span));
}

}  // namespace sonare::midi::synth
