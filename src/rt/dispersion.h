#pragma once

/// @file dispersion.h
/// @brief Shared stiff-string dispersion helpers for waveguide loops.
///
/// A real string is stiff, so its partials stretch sharp to the inharmonic law
/// f_n = n*f0*sqrt(1 + B*n^2). A cascade of first-order allpasses inside the
/// loop makes high frequencies travel faster and realizes that stretch. These
/// helpers solve the per-stage allpass coefficient from the inharmonicity
/// coefficient B and report the exact phase delays needed to keep the loop's
/// fundamental tuning accurate. Used by the piano string core and reused by the
/// Karplus-Strong core for steel-string inharmonicity (weaker than a piano).
///
/// RT-safe: bounded, allocation-free, deterministic.

#include <algorithm>
#include <cmath>

namespace sonare::rt {

inline constexpr float kDispPi = 3.14159265358979323846f;
inline constexpr float kDispTwoPi = 6.28318530717958647692f;

/// Exact phase delay (samples) of the first-order allpass
/// H(z) = (a + z^-1)/(1 + a z^-1) at normalized frequency @p w.
inline float allpass_phase_delay(float a, float w) noexcept {
  const float sinw = std::sin(w);
  const float cosw = std::cos(w);
  const float phi = std::atan2(-sinw, a + cosw) - std::atan2(-a * sinw, 1.0f + a * cosw);
  return -phi / std::max(w, 1.0e-6f);
}

/// Phase delay (samples) of the one-pole loop lowpass y = (1-a)x + a*y^-1 at
/// normalized frequency @p w.
inline float onepole_phase_delay(float a, float w) noexcept {
  return std::atan2(a * std::sin(w), 1.0f - a * std::cos(w)) / std::max(w, 1.0e-6f);
}

/// First-order allpass coefficient a (<= 0) for a cascade of @p stages that
/// disperses the waveguide loop into the stiff-string law f_n =
/// n*f0*sqrt(1 + B*n^2). The loop resonates where the total round-trip phase
/// delay equals an integer number of periods, and only the lowpass and the
/// allpass cascade vary the phase delay with frequency, so a is solved
/// (bisection) to supply the stiff-string phase-delay differential between
/// the fundamental and a high reference partial, then clamped so the
/// per-stage delay still fits the loop budget. Endpoint-matched after Rauhala
/// & Valimaki (2006); RT-safe (bounded, allocation-free, deterministic).
inline float dispersion_allpass_a(float b_coeff, float w0, float lp_a, int stages,
                                  float phase_budget) noexcept {
  if (b_coeff <= 0.0f || stages <= 0) return 0.0f;
  // Reference partial: high enough for a measurable differential but shrunk
  // until its stiff-string frequency sits safely below Nyquist (so the
  // treble, where B is large, still gets dispersion instead of bailing out).
  const float n_max = 0.8f * kDispPi / std::max(w0, 1.0e-6f);
  int n_ref = std::clamp(static_cast<int>(n_max), 2, 12);
  while (n_ref > 2 && w0 * static_cast<float>(n_ref) *
                              std::sqrt(1.0f + b_coeff * static_cast<float>(n_ref) *
                                                   static_cast<float>(n_ref)) >=
                          0.9f * kDispPi)
    --n_ref;
  const float fr = static_cast<float>(n_ref);
  const float w1 = w0 * std::sqrt(1.0f + b_coeff);
  const float wr = w0 * fr * std::sqrt(1.0f + b_coeff * fr * fr);
  if (wr >= 0.97f * kDispPi) return 0.0f;
  const float period = kDispTwoPi / w0;
  // Total phase-delay differential the dispersion must realize between the
  // two partials, net of the (frequency-independent) delay line.
  const float total_diff =
      period * (1.0f / std::sqrt(1.0f + b_coeff) - 1.0f / std::sqrt(1.0f + b_coeff * fr * fr));
  const float lp_diff = onepole_phase_delay(lp_a, w1) - onepole_phase_delay(lp_a, wr);
  const float need = (total_diff - lp_diff) / static_cast<float>(stages);
  if (need <= 0.0f) return 0.0f;
  // p_ap(w1;a) - p_ap(wr;a) increases monotonically as a -> -1.
  float lo = -0.999f;
  float hi = 0.0f;
  for (int it = 0; it < 40; ++it) {
    const float a = 0.5f * (lo + hi);
    const float diff = allpass_phase_delay(a, w1) - allpass_phase_delay(a, wr);
    if (diff > need)
      lo = a;
    else
      hi = a;
  }
  float a = 0.5f * (lo + hi);
  // Clamp so the per-stage phase delay at the fundamental fits the loop
  // budget (the delay line must keep a few samples).
  const float max_pap = phase_budget / static_cast<float>(stages);
  if (max_pap > 1.0f && allpass_phase_delay(a, w1) > max_pap) {
    float blo = a;
    float bhi = 0.0f;
    for (int it = 0; it < 30; ++it) {
      const float c = 0.5f * (blo + bhi);
      if (allpass_phase_delay(c, w1) > max_pap)
        blo = c;
      else
        bhi = c;
    }
    a = bhi;
  }
  return a;
}

}  // namespace sonare::rt
