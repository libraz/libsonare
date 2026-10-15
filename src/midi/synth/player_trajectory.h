#pragma once

/// @file player_trajectory.h
/// @brief The time shape of a note's excitation onset, shared by every
///        sustained engine: the mouth pressure of a wind or reed, the bellows
///        of a free reed, the bow speed of a bowed string, the voice of a vocal.
///
/// OnsetContour ramps a dimensionless level toward 1 while the player sustains
/// and toward 0 once released, as a one-pole at a time constant given in
/// milliseconds; the bowed string's constant-acceleration attack is the one
/// alternative form. Each engine multiplies the level in where it always has.
///
/// ramp_coeff() is the one-pole time constant every sustained engine in this
/// bank uses to smooth a control toward its target.

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
inline float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

/// The onset envelope a sustained engine ramps toward 1 while the player
/// sustains and toward 0 once released.
struct OnsetContour {
  float attack_coeff = 0.0f;
  float release_coeff = 0.0f;
  float level = 0.0f;
  bool releasing = false;
  /// Per-sample rate of a linear attack (0 = the one-pole attack). Never used
  /// on the release.
  float attack_rate = 0.0f;

  /// Note-on: silent and sustaining, with the attack and release time
  /// constants built for @p sample_rate. A non-zero @p linear_attack_ms makes
  /// the attack a constant-acceleration ramp reaching 1 in that time.
  void start(float attack_ms, float release_ms, double sample_rate,
             float linear_attack_ms = 0.0f) noexcept {
    level = 0.0f;
    releasing = false;
    attack_coeff = ramp_coeff(attack_ms, sample_rate);
    release_coeff = ramp_coeff(release_ms, sample_rate);
    attack_rate =
        linear_attack_ms > 0.0f
            ? 1.0f / std::max(1.0f, linear_attack_ms * 0.001f * static_cast<float>(sample_rate))
            : 0.0f;
  }

  /// Ramps level toward 1 (sustaining) or 0 (released), at attack_coeff or
  /// release_coeff depending on which state it is currently ramping toward,
  /// and returns it.
  float advance() noexcept {
    if (!releasing && attack_rate > 0.0f) {
      // Constant-acceleration ramp: linear, not the one-pole's peak-at-t=0 decay.
      level = std::min(1.0f, level + attack_rate);
      return level;
    }
    const float target = releasing ? 0.0f : 1.0f;
    const float coeff = releasing ? release_coeff : attack_coeff;
    level += coeff * (target - level);
    return level;
  }

  /// The share of an active injection a bore may take: exactly 1 while blowing, the breath level
  /// once released, so a source that does not depend on the level on its own cannot outlast it.
  float gate() const noexcept { return releasing ? level : 1.0f; }

  /// Note-off: the envelope starts ramping down at release_coeff.
  void release() noexcept { releasing = true; }

  /// Immediate silence.
  void kill() noexcept {
    level = 0.0f;
    releasing = true;
  }
};

}  // namespace sonare::midi::synth
