#pragma once

/// @file wind_breath.h
/// @brief The breath-envelope and excitation-axis storage shared by the
///        sustained wind and reed engines: BreathContour ramps toward blowing
///        or released, ExcitationBases holds the force/brightness base a
///        patch or CC sets plus the mod-matrix offset on top of it.
///
/// Both are pure storage. Each engine keeps its own SONARE_TUNABLE defaults,
/// its own clamp (or lack of one) at the point a base is stored, and its own
/// note-on initialisation — the same discipline string_loop.h applies to the
/// travelling-wave loop it shares.
///
/// ramp_coeff() is the one-pole time constant every sustained engine in this
/// bank uses to smooth a control toward its target.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "midi/synth/excitation_axes.h"

namespace sonare::midi::synth {

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
inline float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

/// The mouth-pressure / bellows envelope a sustained wind or reed engine ramps
/// toward 1 while blowing and toward 0 once released.
struct BreathContour {
  float attack_coeff = 0.0f;
  float release_coeff = 0.0f;
  float level = 0.0f;
  bool releasing = false;

  /// Ramps level toward 1 (blowing) or 0 (released), at attack_coeff or
  /// release_coeff depending on which state it is currently ramping toward.
  void advance() noexcept {
    const float target = releasing ? 0.0f : 1.0f;
    const float coeff = releasing ? release_coeff : attack_coeff;
    level += coeff * (target - level);
  }

  /// Note-off: the envelope starts ramping down at release_coeff.
  void release() noexcept { releasing = true; }

  /// Immediate silence.
  void kill() noexcept {
    level = 0.0f;
    releasing = true;
  }
};

/// Storage for one engine's force/brightness excitation axes: the base a
/// patch or CC sets, and the mod-matrix offset composed on top of it.
///
/// Deliberately does not clamp on store — which base is clamped where differs
/// per engine (brass leaves brightness to its point of use, reed clamps it
/// here) and a value clamped twice is not the value clamped once.
///
/// The zeros below are placeholders, NOT defaults: every adopting engine sets
/// its own before the first note-on, and they do not agree — the force base
/// starts at 0.7 in brass, 0.55 in flute, 0.6 in reed, 0.0 in pipe organ, and
/// the brightness base at 0.5 in all four. An engine that adopts this struct
/// and drops its own initialiser changes what renders before any note-on.
///
/// The position and morph axes of ExcitationAxes are absent on purpose. The
/// bowed string is the only engine that names kAxisPosition
/// (excitation_axes.h), and it derives slope and beta from the axes rather than
/// storing them, so it keeps its own pair.
struct ExcitationBases {
  float force01_base = 0.0f;
  float bright01_base = 0.0f;
  float force_mod01 = 0.0f;
  float bright_mod01 = 0.0f;

  /// Stores whichever axes @p present names; an axis the caller has not
  /// supplied a controller value for yet is left alone rather than zeroed.
  void set_base(const ExcitationAxes& base, uint32_t present) noexcept {
    if ((present & kAxisForce) != 0u) force01_base = base.force;
    if ((present & kAxisBrightness) != 0u) bright01_base = base.brightness;
  }

  /// Stores the mod-matrix offset for both axes unconditionally.
  void set_mod(const ExcitationAxes& offsets) noexcept {
    force_mod01 = offsets.force;
    bright_mod01 = offsets.brightness;
  }
};

}  // namespace sonare::midi::synth
