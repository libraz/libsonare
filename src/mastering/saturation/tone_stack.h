#pragma once

/// @file tone_stack.h
/// @brief Passive treble-mid-bass tone stack, as the circuit rather than as
///        three independent shelves.
///
/// Yeh & Smith, "Discretization of the '59 Fender Bassman Tone Stack", DAFx-06:
/// the network is a third-order passive RC ladder whose closed-form
/// continuous-time transfer function is symbolic in the component values and the
/// three pot positions, discretized by the bilinear transform.
///
/// Two properties of the real circuit are the reason it is worth modelling as a
/// circuit at all, and neither survives an approximation by independent filters:
///
///   - The controls are not orthogonal. Moving the mid control also moves the
///     treble response, because the mid pot sits in the return path of all three
///     branches.
///   - The network has a large insertion loss (8-12 dB even with every control
///     centred) and a deep mid notch. That loss is why a real amp needs so much
///     preamp gain in front of the stack, and reproducing it is what makes a
///     cascaded preamp behave like one.
///
/// Only the pot positions are user-facing; the component values come from the
/// selected amp voicing.

namespace sonare::mastering::saturation {

/// Which set of component values the ladder is built from. The topology is the
/// same in both; the slope resistor and the treble cap are what differ.
enum class ToneStackModel {
  /// The DAFx-06 reference circuit: 56k slope resistor, 250 pF treble cap. The
  /// deeper, higher mid notch (about -12 dB near 720 Hz with the controls
  /// centred) is the scooped American voice.
  kAmerican = 0,
  /// 33k slope resistor, 470 pF treble cap. The smaller slope resistor passes
  /// more into the bass and mid branches, so the notch is shallower and lower
  /// (about -8.5 dB near 620 Hz) and the whole stack loses less — the
  /// mid-forward British voice.
  kBritish = 1,
};

/// Component values of the ladder. Farads and ohms; kept in double because the
/// coefficients are products of up to three capacitances and three resistances,
/// which spans far more decades than float can carry without losing the small
/// terms.
struct ToneStackComponents {
  double c1, c2, c3;
  double r1, r2, r3, r4;
};

ToneStackComponents tone_stack_components(ToneStackModel model) noexcept;

/// Bilinear-transformed third-order coefficients, normalized so a0 == 1.
struct ToneStackCoeffs {
  double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0;
  double a1 = 0.0, a2 = 0.0, a3 = 0.0;
};

/// @brief Designs the discrete filter for one set of pot positions.
/// @param components Ladder component values.
/// @param sample_rate Rate the filter will run at. Designing at an oversampled
///        rate is strictly more accurate here: the bilinear transform's warping
///        error reaches a few dB in the top octave at 20-44 kHz, and is
///        negligible across the audio band at four times that.
/// @param treble,mid,bass Normalized pot positions, each clamped to [0, 1].
///        These are the raw wiper fractions; any taper (the bass pot is
///        logarithmic on the real circuit) is the caller's to apply.
/// @details Allocation-free, branch-free and transcendental-free, so it is safe
///        to run from the audio thread on a parameter change — which is where it
///        is actually reached from, since the amp's tone controls are
///        automatable. It is still about sixty multiplies and a divide, so it
///        belongs on a parameter change and not in a per-sample loop.
///
///        The result is always stable: the ladder is a passive RC network, so
///        its three poles are real and negative for every control setting, and
///        the denominator's dependence excludes the treble control entirely (the
///        treble pot moves zeros only).
ToneStackCoeffs design_tone_stack(const ToneStackComponents& components, double sample_rate,
                                  double treble, double mid, double bass) noexcept;

/// Third-order transposed direct form II. Kept in double: the lowest pole sits
/// at z ~ 0.9997 at an oversampled rate, which a third-order direct form in
/// float does not resolve comfortably.
struct ToneStackState {
  double s1 = 0.0, s2 = 0.0, s3 = 0.0;

  void clear() noexcept { s1 = s2 = s3 = 0.0; }

  float process(float x, const ToneStackCoeffs& c) noexcept {
    const double in = static_cast<double>(x);
    const double y = c.b0 * in + s1;
    s1 = c.b1 * in - c.a1 * y + s2;
    s2 = c.b2 * in - c.a2 * y + s3;
    s3 = c.b3 * in - c.a3 * y;
    return static_cast<float>(y);
  }
};

}  // namespace sonare::mastering::saturation
