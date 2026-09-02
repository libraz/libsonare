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

namespace string_loop_detail {

/// A pole this close to the unit circle already rings for minutes; past it the
/// loop is numerically a resonator rather than a string.
inline constexpr float kMaxPole = 0.995f;

/// The root of a^2 + beta*a + 1 == 0 inside the unit circle. The two roots are
/// reciprocals, so one always is; a complex pair collapses to -beta/2.
inline float stable_pole(float beta) noexcept {
  const float disc = beta * beta - 4.0f;
  if (disc <= 0.0f) return std::clamp(-0.5f * beta, -kMaxPole, kMaxPole);
  const float root = std::sqrt(disc);
  const float hi = 0.5f * (-beta + root);
  const float lo = 0.5f * (-beta - root);
  return std::clamp(std::abs(hi) < std::abs(lo) ? hi : lo, -kMaxPole, kMaxPole);
}

/// |H(w)| of the loss one-pole y += (1-a)(x-y), given cos(w).
inline float onepole_magnitude(float a, float cos_w) noexcept {
  return (1.0f - a) / std::sqrt(std::max(1.0e-12f, 1.0f - 2.0f * a * cos_w + a * a));
}

}  // namespace string_loop_detail

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
/// sin(omega_ref/2)/sin(omega0/2) per traversal — and an unreachable request
/// costs the tilt, never the note: the fundamental keeps the gain it asked for
/// and the reference partial lands wherever one pole could reach.
///
/// That priority is the whole of the second clamp below. The pole attenuates the
/// fundamental as well, so the gain in front of it is scaled up to compensate,
/// and the compensation is bounded — which means the pole is too. Left
/// unbounded it clamps instead, and the pole's own loss then lands on the note
/// as a second decay nothing asked for: a harpsichord's 4' choir fell to 2 ms of
/// a requested 8 s above f'', and every plucked string lost most of its ring
/// over the same span. Neither is visible from here — both loops still tune,
/// still sound and still stay inside the unit circle.
inline StringLoopFilter solve_string_loop_filter(float omega0, float omega_ref, float g_fundamental,
                                                 float g_reference) noexcept {
  /// The loop's peak response (at DC, for a lowpass pole) must stay under one or
  /// the delay line grows without bound.
  constexpr float kMaxLoopGain = 0.9999f;
  /// How many times longer than the fundamental the frequencies under it may
  /// ring. A string has no mode down there, but a lowpass in the loop peaks at
  /// DC, so the gain that holds the fundamental's t60 always leaves something
  /// beneath it ringing longer — 837 s under a 6.8 s note, at the point the
  /// compensation clamped. The bank's own working cases sit at 1.7 times.
  constexpr float kMaxSubFundamentalRing = 8.0f;

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

  const float r2 = ratio * ratio;
  // |H(w0)|/|H(w_ref)| == ratio reduces to a^2 + beta*a + 1 == 0, whose two
  // roots are reciprocals — the stable one is the root inside the unit circle.
  out.a = string_loop_detail::stable_pole(-2.0f * (std::cos(omega_ref) - r2 * c1) / (1.0f - r2));

  // The darkest pole the compensation can still pay for, from the same quadratic
  // — the response the fundamental needs is what bounds |H(w0)| from below.
  const float g_max = std::min(kMaxLoopGain, std::pow(g0, 1.0f / kMaxSubFundamentalRing));
  const float mag_floor = g_max > 0.0f ? g0 / g_max : 1.0f;
  if (mag_floor < 0.999999f) {
    const float m2 = mag_floor * mag_floor;
    out.a =
        std::min(out.a, string_loop_detail::stable_pole(-2.0f * (1.0f - m2 * c1) / (1.0f - m2)));
  } else {
    // A decay already at the loop's ceiling leaves nothing to compensate with,
    // so the only pole that keeps the fundamental's gain is no pole at all.
    out.a = std::min(out.a, 0.0f);
  }

  // Scale the pole back up so the fundamental keeps exactly the gain it was
  // asked for; without this the pole's own attenuation at w0 is an unaccounted
  // second decay.
  out.g = std::min(kMaxLoopGain,
                   g0 / std::max(1.0e-6f, string_loop_detail::onepole_magnitude(out.a, c1)));
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
