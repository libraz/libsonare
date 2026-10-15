#pragma once

/// @file contact_force.h
/// @brief The spectrum of a struck resonator's contact force, shared by the
///        percussion and modal cores.
///
/// A stick, beater, hammer or hand applies a smooth force pulse over its
/// contact time, and that pulse's spectrum read at each mode's frequency is the
/// mode's share of the strike: the standard modal reduction, in which the force
/// does not depend on the resonator state (Chaigne & Doutaut, JASA 101, 1997).

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

/// Magnitude spectrum of a raised-cosine contact force of duration tau, read at
/// x = f * tau and normalised to unity at DC: |sinc(x) / (1 - x^2)|.
inline float contact_spectrum(float x) noexcept {
  const float ax = std::abs(x);
  if (ax < 1.0e-6f) return 1.0f;
  // sinc and the denominator go to zero together at x = 1, where the limit is
  // 1/2 and the quotient is two cancellations divided by each other.
  if (std::abs(ax - 1.0f) < 1.0e-3f) return 0.5f;
  const float s = std::sin(constants::kPi * ax) / (constants::kPi * ax);
  return std::abs(s / (1.0f - ax * ax));
}

/// Samples a contact pulse of @p contact_ms spans: one more than its period,
/// since the pulse runs over [0, 1] inclusive and the knob keeps its unit.
inline int contact_pulse_samples(float contact_ms, double sample_rate) noexcept {
  const float period = std::max(0.001f, contact_ms) * 0.001f * static_cast<float>(sample_rate);
  return std::max(2, 1 + static_cast<int>(std::lround(period)));
}

/// The contact's direct radiation: one period of a sine over the contact time,
/// the derivative of a raised-cosine force, so its energy peaks at 1/contact_ms.
/// It reaches the listener past the voice's drive, filter and envelope.
struct ContactPulse {
  float level = 0.0f;
  int i = 0;
  int len = 0;

  /// Arms the pulse at @p pulse_level; 0 leaves it silent and skipped.
  void configure(float pulse_level, int samples) noexcept {
    level = pulse_level;
    i = 0;
    len = level > 0.0f ? samples : 0;
  }
  float next() noexcept {
    if (i >= len) return 0.0f;
    // Spanning [0, 1] inclusive, so the pulse both starts and ends on a zero.
    // Dividing by the length instead leaves the last sample short of the
    // period and the step it ends on is a click with energy to Nyquist, which
    // lands nowhere near the contact time it is supposed to be voiced at.
    const float p = static_cast<float>(i++) / static_cast<float>(len - 1);
    return level * std::sin(constants::kTwoPi * p);
  }
  void stop() noexcept {
    level = 0.0f;
    i = len;
  }
};

}  // namespace sonare::midi::synth
