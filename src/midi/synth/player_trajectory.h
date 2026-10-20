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
/// A tongued onset can carry an overshoot: the one-pole's target starts above 1
/// and settles back onto it, which is exactly the plain ramp at 0 dB.
///
/// ramp_coeff() is the one-pole time constant every sustained engine in this
/// bank uses to smooth a control toward its target.

#include <algorithm>
#include <cmath>

#include "midi/synth/envelope.h"
#include "util/db.h"

namespace sonare::midi::synth {

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
inline float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

/// Level under which a released OnsetContour is exactly zero: a hundredth of the amplitude
/// envelope's floor, so the snap stays far below anything it could shape.
inline constexpr float kOnsetSilenceLevel = 0.01f * DahdsrEnvelope::kSilenceLevel;

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
  /// How far the one-pole attack's target stands above 1, decaying toward 0 at
  /// overshoot_decay per sample while sustaining (0 = the plain ramp).
  float overshoot = 0.0f;
  float overshoot_decay = 0.0f;

  /// Note-on: silent and sustaining, with the attack and release time
  /// constants built for @p sample_rate. A non-zero @p linear_attack_ms makes
  /// the attack a constant-acceleration ramp reaching 1 in that time.
  void start(float attack_ms, float release_ms, double sample_rate,
             float linear_attack_ms = 0.0f) noexcept {
    level = 0.0f;
    releasing = false;
    overshoot = 0.0f;
    attack_coeff = ramp_coeff(attack_ms, sample_rate);
    release_coeff = ramp_coeff(release_ms, sample_rate);
    attack_rate =
        linear_attack_ms > 0.0f
            ? 1.0f / std::max(1.0f, linear_attack_ms * 0.001f * static_cast<float>(sample_rate))
            : 0.0f;
  }

  /// The player's tongued onset on the one-pole attack: the target starts
  /// @p overshoot_db above the held level and settles back onto it, ~95% of
  /// the way in @p settle_ms. Call after start(); 0 dB leaves the ramp as it was.
  void set_overshoot(float overshoot_db, float settle_ms, double sample_rate) noexcept {
    overshoot = overshoot_db > 0.0f ? sonare::db_to_linear(overshoot_db) - 1.0f : 0.0f;
    overshoot_decay = ramp_coeff(settle_ms, sample_rate);
  }

  /// set_overshoot() read down from velocity 127: @p overshoot_db at 127, moved by
  /// @p per_doubling_db for each halving of @p vel01. 0 dB at 127 is off.
  void set_overshoot_at_velocity(float overshoot_db, float per_doubling_db, float vel01,
                                 float settle_ms, double sample_rate) noexcept {
    const float db =
        overshoot_db > 0.0f
            ? overshoot_db + per_doubling_db * std::log2(std::max(vel01, 1.0f / 127.0f))
            : 0.0f;
    set_overshoot(db, settle_ms, sample_rate);
  }

  /// Drops what is left of the overshoot, for an onset some input now drives.
  void hold_overshoot() noexcept { overshoot = 0.0f; }

  /// The overshoot's target over the held one: exactly 1 once it has settled.
  float overshoot_ratio() const noexcept { return 1.0f + overshoot; }

  /// Ramps level toward 1 (sustaining) or 0 (released), at attack_coeff or
  /// release_coeff depending on which state it is currently ramping toward,
  /// and returns it.
  float advance() noexcept {
    if (!releasing && attack_rate > 0.0f) {
      // Constant-acceleration ramp: linear, not the one-pole's peak-at-t=0 decay.
      level = std::min(1.0f, level + attack_rate);
      return level;
    }
    float target = releasing ? 0.0f : 1.0f;
    if (overshoot > 0.0f && !releasing) {
      target += overshoot;
      overshoot -= overshoot_decay * overshoot;
      if (overshoot < 1.0e-6f) overshoot = 0.0f;
    }
    const float coeff = releasing ? release_coeff : attack_coeff;
    level += coeff * (target - level);
    // Below the floor a release is silence; left to decay it would park on a subnormal.
    if (releasing && level <= kOnsetSilenceLevel) level = 0.0f;
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
