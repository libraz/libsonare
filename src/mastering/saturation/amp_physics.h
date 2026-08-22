#pragma once

/// @file amp_physics.h
/// @brief The amp-sim controls that have a physical anchor, and the derivations
///        that produce them.
///
/// Most of the amp's controls are normalized [0, 1] abstractions, and a
/// normalized control cannot be justified by a citation — there is no reference
/// that says a rig should sit at `drive = 0.62`. Three of them are different:
/// they stand for quantities a real amp is actually specified by, so they can be
/// derived instead of chosen. This file is where that derivation lives, so the
/// constants stay auditable rather than being buried as magic numbers in the
/// signal path.
///
/// What is deliberately NOT here: `drive`, the three tone controls, `presence`,
/// the mic positions and `cone`. Those are voicing choices with no physical
/// referent in this model, and pretending otherwise would dress taste as
/// measurement.

namespace sonare::mastering::saturation {

enum class PowerTube;

// ---------------------------------------------------------------------------
// Output tubes
// ---------------------------------------------------------------------------

/// @brief Maximum plate dissipation of one output tube, in watts.
/// @details This is the tube-intrinsic figure from the datasheets, and it is the
///          right one to derive headroom from. The obvious alternative — the
///          output power of a push-pull pair — is NOT tube-intrinsic: it is set
///          at least as much by the rail voltage the surrounding circuit chose,
///          which is why a 6L6 pair is quoted anywhere between 24 W and 45 W
///          depending on the amp it sits in. Dissipation does not move with the
///          circuit.
float plate_dissipation_w(PowerTube tube) noexcept;

/// Reference dissipation the drive scale is normalized against: the 6L6GC, which
/// is therefore exactly 1.0 and keeps the default power stage unchanged.
inline constexpr float kReferencePlateDissipationW = 30.0f;

/// @brief Drive scale into the power stage for an output tube.
/// @details A tube reaches its rails at an output voltage set by how much power
///          it can pass, and power goes as voltage squared — so the drive needed
///          to reach the rails scales as `sqrt(P_ref / P_tube)`. A smaller bottle
///          therefore breaks up earlier at the same setting, which is the whole
///          audible content of the choice.
float power_tube_scale(PowerTube tube) noexcept;

// ---------------------------------------------------------------------------
// Bias point
// ---------------------------------------------------------------------------

/// Idle dissipation fraction at or above which the pair conducts through the
/// zero crossing on musical signals, so no crossover notch is audible. Amp
/// practice calls this a "hot" bias.
inline constexpr float kClassABHotBiasFraction = 0.70f;
/// The coldest bias in common practice; below this an amp is being run outside
/// what anyone would set deliberately.
inline constexpr float kClassABColdBiasFraction = 0.30f;

/// @brief Converts a real amp's bias point into the `crossover` control.
/// @param bias_fraction Idle plate dissipation as a fraction of the tube's
///        maximum — the number an amp tech actually sets. Conventional practice
///        is roughly 0.5 for a cool class-AB setting, 0.6 warm, and 0.7-0.75
///        hot.
/// @return The `crossover` control in [0, 1]: 0 at or above a hot bias (no dead
///         zone), 1 at or below the coldest, linear between.
/// @details Linear between the two anchors rather than fitted: the dead zone's
///          width grows with how far below conduction each tube idles, and no
///          reference this model could be held to specifies that curve. The
///          anchors are the defensible part; the interpolation is a choice, and
///          is documented as one.
float crossover_from_bias_fraction(float bias_fraction) noexcept;

// ---------------------------------------------------------------------------
// Power supply
// ---------------------------------------------------------------------------

/// Rectifier type, which is what sets how far the rail collapses under load.
/// The values are the tube's effective internal series resistance in ohms.
enum class Rectifier {
  /// Silicon diodes: essentially no drop, so essentially no sag.
  kSolidState = 0,
  /// 5AR4 / GZ34: the stiffest of the common tube rectifiers.
  kGz34 = 1,
  /// 5V4.
  k5V4 = 2,
  /// 5U4.
  k5U4 = 3,
  /// 5Y3: the softest, and the reason small tweed amps sag the way they do.
  k5Y3 = 4,
};

/// @brief Effective internal series resistance of a rectifier, in ohms.
/// @details Commonly cited ballpark figures; a rectifier's drop is conditional
///          on the test current, so these are the right order of magnitude and
///          the right ordering rather than a specification.
float rectifier_resistance_ohms(Rectifier rectifier) noexcept;

/// Representative DC resistance of a guitar-amp power transformer's high-voltage
/// secondary, in ohms. It is in series with the rectifier and is NOT negligible:
/// leaving it out understates the rail drop by several times, and putting it in
/// is what makes the derivation reproduce the drops these amps are actually
/// measured at.
inline constexpr float kTypicalPowerTransformerOhms = 150.0f;

/// @brief Derives the `sag` control from a supply's own numbers.
/// @param supply_ohms TOTAL series resistance ahead of the reservoir — the
///        rectifier plus the transformer secondary, not the rectifier alone.
/// @param full_output_current_a Plate current the output pair draws at full
///        output.
/// @param b_plus_v The supply's no-load rail voltage.
/// @return The fractional rail droop at full output, which is exactly what the
///         `sag` control means. Clamped to [0, 1].
/// @details Ohm's law on the supply: the rail falls by `I * R` out of `B+`. The
///          derivation reproduces both drops these amps are commonly quoted at —
///          a 5Y3 small combo (350 + 150 ohms, 100 mA, 350 V) gives 50 V, and a
///          GZ34 big combo (50 + 150 ohms, 150 mA, 440 V) gives 30 V — which is
///          the check that the model is the right one rather than merely a
///          plausible formula.
float sag_from_supply(float supply_ohms, float full_output_current_a, float b_plus_v) noexcept;

}  // namespace sonare::mastering::saturation
