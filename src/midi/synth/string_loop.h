#pragma once

/// @file string_loop.h
/// @brief One travelling-wave string loop: a fractional-delay line closed
///        through a one-pole loss lowpass.
///
/// A plucked instrument is rarely one loop. A guitar has its second
/// polarization, a harpsichord its 4' companion choir and its behind-the-bridge
/// segment. Each is this same skeleton at a different period, brightness and
/// t60, and each used to be written out again field by field inside whichever
/// core needed it. This is that skeleton once, for the cores that share it
/// (ks_voice.h and harpsichord_voice.h).
///
/// What the loop deliberately does NOT own is anything that happens *inside* it
/// for one instrument only: the in-loop dispersion allpass, the fret-slap
/// limiter, the buzzing bridge. Those live at the call site, which reads the
/// delayed sample with advance(), shapes it, and hands it back through commit().
/// A loop with nothing to shape uses process(), which is the two in sequence.
///
/// The delay buffer is NOT owned: the host instrument allocates one slab per
/// voice slot in prepare() (the only allocation site) and configure() carves a
/// span out of it. configure() zeroes the span it takes; nothing else allocates.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s: the
/// -60 dB is spread across however many loop traversals fit in that time.
inline float string_loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  return std::exp(-6.907755279f / loops_to_t60);
}

/// A solved one-pole loss filter: the feedback coefficient and the gain in
/// front of it, in the form StringLoop::configure_filter() takes.
struct StringLoopFilter {
  float a = 0.0f;
  float g = 0.0f;
};

/// Solves the loop's loss filter from what the string has to DO rather than
/// from a tone knob: @p g_fundamental is the per-traversal gain the fundamental
/// (@p omega0, radians per sample) must keep, and @p g_reference the smaller one
/// the partial at @p omega_ref keeps.
///
/// Setting the response at two named frequencies is what makes a decay target
/// mean the same thing at every pitch. A one-pole picked for its DC gain is
/// already attenuating a treble fundamental on every traversal, which at over a
/// thousand traversals a second overwhelms whatever t60 was asked for.
///
/// WHERE the second point sits matters more than the compensation does. Quote
/// the damping at the octave and a bass string needs a pole at 0.94 to tilt at
/// all — two frequencies a third of a percent of the sample rate apart barely
/// differ to a one-pole — and that same pole is a brick wall four octaves up, so
/// the string loses everything above its tenth partial and sounds like a sine.
/// Quote it at a fixed frequency instead, the way string damping is actually
/// measured, the pole stays small, and the harmonics survive. Measured on the
/// harpsichord voicing, the compensation below is then worth 3.4 dB/s at f''' and
/// under 0.5 anywhere beneath it; a pole chosen for tone and left uncompensated
/// costs that same f''' about 62 dB/s, which is what kills a treble string.
///
/// A single pole can only tilt so far — the reachable ratio tops out at
/// sin(omega_ref/2)/sin(omega0/2) per traversal — so an unreachable request
/// clamps to the most damping one pole has rather than failing.
inline StringLoopFilter solve_string_loop_filter(float omega0, float omega_ref, float g_fundamental,
                                                 float g_reference) noexcept {
  /// A pole this close to the unit circle already rings for minutes; past it the
  /// loop is numerically a resonator rather than a string.
  constexpr float kMaxPole = 0.995f;
  /// The loop's peak response (at DC, for a lowpass pole) must stay under one or
  /// the delay line grows without bound.
  constexpr float kMaxLoopGain = 0.9999f;

  StringLoopFilter out;
  const float g0 = std::clamp(g_fundamental, 0.0f, kMaxLoopGain);
  const float ratio = g_reference > 0.0f ? g0 / g_reference : 1.0f;
  const float c1 = std::cos(omega0);

  if (!(ratio > 1.000001f)) {
    // The two targets agree: no tilt to build, so the pole is transparent.
    out.a = 0.0f;
    out.g = g0;
    return out;
  }

  const float c2 = std::cos(omega_ref);
  const float r2 = ratio * ratio;
  const float denom = 1.0f - r2;
  // |H(w0)|/|H(w_ref)| == ratio reduces to a^2 + beta*a + 1 == 0, whose two
  // roots are reciprocals — the stable one is the root inside the unit circle.
  const float beta = -2.0f * (c2 - r2 * c1) / denom;
  const float disc = beta * beta - 4.0f;
  if (disc <= 0.0f) {
    // Past what one pole can tilt; take the most damping it has.
    out.a = std::clamp(-0.5f * beta, -kMaxPole, kMaxPole);
  } else {
    const float root = std::sqrt(disc);
    const float hi = 0.5f * (-beta + root);
    const float lo = 0.5f * (-beta - root);
    out.a = std::clamp(std::abs(hi) < std::abs(lo) ? hi : lo, -kMaxPole, kMaxPole);
  }

  // Scale the pole back up so the fundamental keeps exactly the gain it was
  // asked for; without this the pole's own attenuation at w0 is an unaccounted
  // second decay.
  const float mag_at_w0 =
      (1.0f - out.a) / std::sqrt(std::max(1.0e-12f, 1.0f - 2.0f * out.a * c1 + out.a * out.a));
  out.g = std::min(kMaxLoopGain, g0 / std::max(1.0e-6f, mag_at_w0));
  return out;
}

/// One string loop: a circular delay line read at a fractional offset and closed
/// through a one-pole loss filter and a per-traversal gain.
struct StringLoop {
  /// Delay line (a span of the voice's slab), and the span length actually used
  /// for the current note — the period plus bend-down headroom and the
  /// interpolator's stencil margin.
  float* buffer = nullptr;
  int size = 0;
  size_t write = 0;

  /// Ideal loop period in samples at ratio == 1.
  float period = 0.0f;
  /// Loop delay NOT in the delay line: the one-sample feedback path plus the
  /// loss filter's phase delay at the fundamental. A call site with an in-loop
  /// allpass adds its phase delay here after configure().
  float loop_comp = 1.0f;

  /// Loss lowpass y += alpha * (x - y), and its state.
  float alpha = 1.0f;
  float lp_state = 0.0f;

  /// Per-traversal amplitude factor for the sounding t60, and the one release()
  /// re-targets it to (the damper).
  float gain = 0.0f;
  float release_gain = 0.0f;

  /// Sets the loop up for a note. @p a is the loss filter's feedback coefficient
  /// (the filter is y += (1-a)(x-y), so a == 0 is transparent and larger a is
  /// darker). @p capacity is the span length available in the slab.
  void configure(float* slab, int capacity, float period_samples, double sample_rate, float a,
                 float t60_s, float release_t60_s) noexcept {
    configure_filter(slab, capacity, period_samples, a,
                     string_loop_gain_for(period_samples, sample_rate, t60_s),
                     string_loop_gain_for(period_samples, sample_rate, release_t60_s));
  }

  /// Sets the loop up from an already-solved loss filter: @p a is the one-pole's
  /// feedback coefficient and @p g the per-traversal gain in front of it.
  ///
  /// Use this when the filter was designed against decay targets at named
  /// frequencies rather than from a brightness knob. The distinction matters in
  /// the treble: a one-pole with unity DC gain still attenuates a 1.4 kHz
  /// fundamental on every traversal, and at 1400 traversals a second that loss
  /// dwarfs the nominal t60, so a string built from a brightness knob loses its
  /// top octave no matter what decay it was asked for.
  void configure_filter(float* slab, int capacity, float period_samples, float a, float g,
                        float release_g) noexcept {
    buffer = slab;
    period = period_samples;
    write = 0;
    alpha = 1.0f - a;
    lp_state = 0.0f;
    loop_comp = 1.0f + onepole_group_delay_samples(a, constants::kTwoPi / period_samples);
    gain = g;
    release_gain = release_g;
    size = std::min(capacity, static_cast<int>(period_samples * 1.3f) + 8);
    if (buffer != nullptr) {
      std::fill(buffer, buffer + static_cast<size_t>(std::max(0, size)), 0.0f);
    }
  }

  /// Leaves the loop silent and skipped: a call site gates on gain or on its own
  /// mix level, and a disengaged loop must not carry state from the last note.
  void disable() noexcept {
    size = 0;
    write = 0;
    lp_state = 0.0f;
    gain = 0.0f;
  }

  /// Writes @p input into the line and reads the delayed sample back. @p ratio is
  /// the per-sample pitch factor (bend / vibrato / tension), 1 = on pitch; it
  /// scales the frequency, so it divides the delay. The returned value is the
  /// string's output BEFORE the loss filter — shape it if the instrument shapes
  /// it, then hand it to commit().
  float advance(float input, float ratio) noexcept {
    const float delay = std::clamp(period / ratio - loop_comp, 1.0f, static_cast<float>(size - 4));
    const int delay_q8 = static_cast<int>(delay * 256.0f);
    return rt::lagrange3_fractional_delay(buffer, static_cast<size_t>(size), write, delay_q8,
                                          input);
  }

  /// Closes the loop: the (possibly shaped) delayed sample enters the loss filter.
  void commit(float shaped) noexcept { lp_state += alpha * (shaped - lp_state); }

  /// advance() then commit(), for a loop with nothing shaped inside it.
  float process(float input, float ratio) noexcept {
    const float out = advance(input, ratio);
    commit(out);
    return out;
  }

  /// The feedback term to add into the next sample's loop input.
  float feedback() const noexcept { return gain * lp_state; }

  /// Note-off: re-target the decay to the damped t60. Never lengthens a decay
  /// that is already shorter than the damper's.
  void release() noexcept { gain = std::min(gain, release_gain); }

  /// Immediate silence.
  void kill() noexcept {
    gain = 0.0f;
    lp_state = 0.0f;
  }
};

}  // namespace sonare::midi::synth
