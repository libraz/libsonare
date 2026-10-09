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
#include <array>
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

/// The sample rate every shipped loss coefficient and seeded noise level in
/// this bank was voiced at.
constexpr double kLossVoicedSr = 48000.0;

/// Re-expresses a loss pole voiced at @ref kLossVoicedSr so it keeps its corner,
/// in Hz, at @p sample_rate.
///
/// The one-pole `y += (1-a)(x-y)` decays by `a` per SAMPLE, so its corner sits
/// at `-ln(a)*sr/(2*pi)`: holding `a` fixed moves the corner with the rate, and
/// holding the corner fixed is `a^(kLossVoicedSr/sr)`. Exact at the voiced rate,
/// and within 0.05 dB of the voiced response up to the corner at 44.1 and 96 kHz
/// (0.47 dB at 10 kHz), against up to 4.4 dB for the unmapped coefficient.
///
/// It takes the rate and NOT the note, which is the whole content of the law: a
/// bore, a bell or a bridge is a property of the instrument and does not change
/// its cutoff when a different note is fingered. A mapping that also took the
/// note would re-voice the register — quoting one note's decay TIME at every
/// other note swings this bank's flute corner from 1681 Hz at note 60 to 4598 Hz
/// at note 90, where the instrument has one corner at 3291 Hz.
inline float loss_pole_at_rate(float a_voiced, double sample_rate) noexcept {
  if (!(sample_rate > 0.0) || a_voiced <= 0.0f) return a_voiced;
  return static_cast<float>(std::pow(static_cast<double>(a_voiced), kLossVoicedSr / sample_rate));
}

/// loss_pole_at_rate() for a loss one-pole written `y += alpha*(x-y)`, taking
/// and returning `alpha = 1 - a`. Computed in double so the voiced rate returns
/// @p alpha_voiced bit for bit, which `1 - loss_pole_at_rate(1 - alpha)` in
/// float does not.
inline float loss_alpha_at_rate(float alpha_voiced, double sample_rate) noexcept {
  if (!(sample_rate > 0.0) || !(alpha_voiced < 1.0f)) return alpha_voiced;
  const double a = 1.0 - static_cast<double>(alpha_voiced);
  return static_cast<float>(1.0 - std::pow(a, kLossVoicedSr / sample_rate));
}

/// Re-expresses a per-sample noise level voiced at @ref kLossVoicedSr so it
/// keeps its power per Hz at @p sample_rate.
///
/// One independent draw per output sample spreads a fixed variance over
/// `0..sr/2`, so the power landing inside any band quoted in Hz -- a bore, a
/// formant, a follower's corner -- falls as `1/sr`. The level is therefore
/// scaled by `sqrt(sr / kLossVoicedSr)`: exactly 1 at the voiced rate, and the
/// realization is still one draw per sample, so a 48 kHz render is unchanged
/// bit for bit.
inline float noise_gain_at_rate(double sample_rate) noexcept {
  if (!(sample_rate > 0.0)) return 1.0f;
  return static_cast<float>(std::sqrt(sample_rate / kLossVoicedSr));
}

/// Exact rate correction for noise shaped by ONE one-pole lowpass whose
/// output goes straight to the mix (no further resonator downstream): holds
/// that pole's own output RMS at @p cutoff_hz equal to what it was at
/// kLossVoicedSr, for any corner-to-Nyquist ratio.
///
/// White noise of unit per-sample variance through y += a(x-y) has output
/// variance a/(2-a). noise_gain_at_rate assumes a is small (the corner sits
/// far below Nyquist) and is only asymptotically correct; this holds exactly
/// at any rate, including a low sample rate where the corner is a sizeable
/// fraction of Nyquist. Exactly 1 at kLossVoicedSr, so a 48 kHz render is
/// still bit-identical.
inline float onepole_noise_rate_gain(float cutoff_hz, double sample_rate) noexcept {
  if (!(sample_rate > 0.0) || !(cutoff_hz > 0.0f)) return 1.0f;
  const auto pole_variance = [](float fc, double sr) noexcept {
    const float a =
        std::clamp(1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sr)), 0.01f, 1.0f);
    return a / (2.0f - a);
  };
  return std::sqrt(pole_variance(cutoff_hz, kLossVoicedSr) / pole_variance(cutoff_hz, sample_rate));
}

/// Input gain of a two-pole resonator `y = a1*y1 + a2*y2 + gain*x` (a1 = 2r cos w, a2 = -r^2) that
/// keeps the response at its centre the same physical size at every sample rate.
///
/// The centre response is gain / d with d = (1-r) * sqrt(cos^2 w (1-r)^2 + sin^2 w (1+r)^2), and d
/// is not proportional to (1-r): at a fixed centre in Hz and a fixed ring time it shrinks as the
/// rate rises, so the customary `gain = 1 - r` lets the same mode answer harder at 96 kHz than at
/// 48 kHz. This returns the gain the resonator voiced at kLossVoicedSr (`1 - r48`) would have,
/// scaled by d / d48; exactly `1 - r` at the voiced rate. @p r and @p w are the radius and centre
/// at @p sample_rate.
inline float resonator_gain_at_rate(float r, float w, double sample_rate) noexcept {
  if (!(sample_rate > 0.0) || sample_rate == kLossVoicedSr) return 1.0f - r;
  const auto denominator = [](double radius, double omega) noexcept {
    const double c = std::cos(omega) * (1.0 - radius);
    const double s = std::sin(omega) * (1.0 + radius);
    return (1.0 - radius) * std::sqrt(c * c + s * s);
  };
  const double scale = sample_rate / kLossVoicedSr;
  const double r_voiced = std::pow(static_cast<double>(r), scale);
  const double w_voiced = static_cast<double>(w) * scale;
  const double d_voiced = denominator(r_voiced, w_voiced);
  if (!(d_voiced > 1.0e-12)) return 1.0f - r;
  return static_cast<float>((1.0 - r_voiced) * denominator(r, w) / d_voiced);
}

/// A solved one-pole loss filter: the feedback coefficient and the gain in
/// front of it, in the form StringLoop::configure_filter() takes.
struct StringLoopFilter {
  float a = 0.0f;
  float g = 0.0f;
};

/// |H(w)| of the loss one-pole y += (1-a)(x-y), given w in radians per sample.
///
/// (1-a)^2 + 4a sin^2(w/2) rather than the textbook 1 - 2a cos w + a^2, whose
/// two terms near 2 leave 6e-5 at the dark poles and low fundamentals this
/// solver reaches — three float digits, and the gain compensating it then misses
/// the fundamental's target by parts in a thousand, differently per libm. Taking
/// cos(w) from the caller would lose the same digits.
///
/// Public: also used to read a shipped pole's own magnitude at a named
/// frequency when converting it into a frequency-referenced law (the anchor
/// step every loop-loss fix takes before it ever calls the solver below).
inline float onepole_magnitude(float a, float omega) noexcept {
  const float half_sin = std::sin(0.5f * omega);
  const float pole_gap = 1.0f - a;
  return pole_gap /
         std::sqrt(std::max(1.0e-12f, pole_gap * pole_gap + 4.0f * a * half_sin * half_sin));
}

namespace string_loop_detail {

/// A pole this close to the unit circle already rings for minutes; past it the
/// loop is numerically a resonator rather than a string.
inline constexpr float kMaxPole = 0.995f;

/// The loop's peak response (at DC, for a lowpass pole) must stay under one or
/// the delay line grows without bound.
inline constexpr float kMaxLoopGain = 0.9999f;

/// How many times longer than the fundamental the frequencies under it may
/// ring, since a lowpass in the loop peaks at DC and the gain holding the
/// fundamental's t60 always leaves something beneath it ringing longer.
inline constexpr float kMaxSubFundamentalRing = 8.0f;

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

}  // namespace string_loop_detail

/// The one-pole feedback coefficient giving |H(omega0)|/|H(omega_ref)| ==
/// @p ratio (ratio > 1), from the quadratic a^2 + beta*a + 1 == 0. No clamp,
/// no compensation: the shape solve_string_loop_filter reuses for both its
/// own tilt solve and its sub-fundamental ring cap, and that a caller
/// composing several poles into a cascade calls directly so the ring bound
/// can be applied once to the cascade rather than to one stage alone.
inline float solve_tilt_pole(float omega0, float omega_ref, float ratio) noexcept {
  const float c1 = std::cos(omega0);
  const float r2 = ratio * ratio;
  return string_loop_detail::stable_pole(-2.0f * (std::cos(omega_ref) - r2 * c1) / (1.0f - r2));
}

/// The darkest per-traversal gain content below the fundamental may keep, so
/// it rings no more than kMaxSubFundamentalRing times as long as a loop whose
/// fundamental itself keeps @p g_fundamental per traversal.
inline float sub_fundamental_gain_cap(float g_fundamental) noexcept {
  return std::min(string_loop_detail::kMaxLoopGain,
                  std::pow(g_fundamental, 1.0f / string_loop_detail::kMaxSubFundamentalRing));
}

/// Gain in front of a solved pole @p a that keeps its response at
/// @p omega_target equal to @p g_target, clamped to the loop's stability
/// ceiling.
inline float compensated_loop_gain(float a, float omega_target, float g_target) noexcept {
  return std::min(string_loop_detail::kMaxLoopGain,
                  g_target / std::max(1.0e-6f, onepole_magnitude(a, omega_target)));
}

/// Caps the requested loss pole @p a at the darkest one whose compensation
/// (compensated_loop_gain) the sub-fundamental ring bound can still pay for
/// at @p omega0 for a fundamental gain @p g0; below it, no pole at all.
inline float cap_loss_pole(float a, float omega0, float g0) noexcept {
  const float g_max = sub_fundamental_gain_cap(g0);
  const float mag_floor = g_max > 0.0f ? g0 / g_max : 1.0f;
  return std::min(a, mag_floor < 0.999999f ? solve_tilt_pole(omega0, 0.0f, mag_floor) : 0.0f);
}

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
  StringLoopFilter out;
  const float g0 = std::clamp(g_fundamental, 0.0f, string_loop_detail::kMaxLoopGain);
  const float ratio = g_reference > 0.0f ? g0 / g_reference : 1.0f;

  if (!(ratio > 1.000001f)) {
    // The two targets agree: no tilt to build, so the pole is transparent.
    out.a = 0.0f;
    out.g = g0;
    return out;
  }

  // |H(w0)|/|H(w_ref)| == ratio reduces to a^2 + beta*a + 1 == 0, whose two
  // roots are reciprocals — the stable one is the root inside the unit circle.
  out.a = solve_tilt_pole(omega0, omega_ref, ratio);

  out.a = cap_loss_pole(out.a, omega0, g0);

  // Scale the pole back up so the fundamental keeps exactly the gain it was
  // asked for; without this the pole's own attenuation at w0 is an unaccounted
  // second decay.
  out.g = compensated_loop_gain(out.a, omega0, g0);
  return out;
}

/// What a delay-line loop of a requested period realises, and what the read costs.
struct LoopBudget {
  /// Delay actually read from the line: the requested period less the delay carried outside it,
  /// raised to the floor the read needs.
  float delay = 1.0f;
  /// Loop period the realised delay sounds (delay + comp): below the requested one exactly when
  /// the floor engaged, which pins every higher note at that pitch unless the voice oversamples.
  float achieved_period = 0.0f;
  bool floored = false;
  /// Per-traversal gain that pays for the Lagrange read's magnitude at the sounding fundamental,
  /// >= 1. Absolute (1/|H|) when no rate is given; with a rate it is the ratio against the same
  /// note read at kLossVoicedSr, so a voice calibrated at that rate keeps its sound there.
  float interp_gain = 1.0f;
  /// Integer factor the loop has to run at, per host sample: 1 whenever the period clears the
  /// floor and is above kMinHeldPeriod. A floored or shorter loop runs at the larger of the
  /// smallest factor an estimate says clears the floor and the one reaching the note's held
  /// rate: kLossVoicedSr, the rate the bank is voiced at, lifted until the period the note has
  /// there clears kMinHeldPeriod (see settle_loop_oversample() for how a voice confirms it
  /// against its real compensation). Without @p sample_rate the host rate is taken as voiced.
  int oversample = 1;
};

/// The shortest loop period, in samples at the rate the loop runs at, that holds its pitch and
/// repays its read: a cylindrical reed at 48 kHz holds within 6 cents down to a 3.2-sample line
/// and sits 10 to 23 cents sharp from 2.7 down, where twice the rate holds the same notes within 4.
inline constexpr float kMinHeldPeriod = 8.0f;

/// The largest oversampling factor a loop is asked to run at. A note needing more than this is far
/// outside the band any supported rate carries and keeps sounding at its floor rather than costing
/// the voice an unbounded multiple of its render.
inline constexpr int kMaxLoopOversample = 8;

/// The delay a loop reads from its line for a requested period: the period less @p comp_samples
/// (what is carried outside the line), raised to @p min_delay. Cheap enough for the sample loop;
/// loop_budget() adds the reporting and the interpolation cost for setup time.
inline float loop_delay(float period_samples, float comp_samples, float min_delay) noexcept {
  return std::max(min_delay, period_samples - comp_samples);
}

/// The one place a requested loop period becomes a realised delay, its floor, the oversampling
/// that lifts the floor and the interpolation loss the loop has to repay. @p comp_samples is the
/// delay carried outside the line (feedback register, loss-filter phase) and @p min_delay the
/// smallest delay the read supports. @p register_samples is the part of @p comp_samples that is a
/// count of samples (the feedback registers) rather than a duration: running the loop k times
/// faster leaves it at that many internal samples while every other term stretches to k times as
/// many. A floored period is reported through achieved_period and oversample instead of being
/// clamped silently.
inline LoopBudget loop_budget(float period_samples, float comp_samples, float min_delay,
                              double sample_rate = 0.0, float register_samples = 1.0f) noexcept {
  LoopBudget out;
  const float wanted = period_samples - comp_samples;
  out.floored = wanted < min_delay;
  out.delay = loop_delay(period_samples, comp_samples, min_delay);
  out.achieved_period = out.delay + comp_samples;
  if (out.floored) {
    // k * period - (register + k * (comp - register)) >= min_delay
    const float room = period_samples - comp_samples + register_samples;
    out.oversample =
        room > 0.0f ? std::clamp(static_cast<int>(std::ceil((min_delay + register_samples) / room)),
                                 2, kMaxLoopOversample)
                    : kMaxLoopOversample;
  }
  if (out.floored || period_samples <= kMinHeldPeriod) {
    // The held rate is the note's alone, so every host rate dividing the voiced one draws it alike.
    const double voiced_period =
        sample_rate > 0.0 ? period_samples * kLossVoicedSr / sample_rate : period_samples;
    const double held_rate_scale = std::ceil(
        kMinHeldPeriod / std::max(voiced_period, static_cast<double>(kMinHeldPeriod) /
                                                     static_cast<double>(kMaxLoopOversample)));
    const double host_scale = sample_rate > 0.0 ? kLossVoicedSr / sample_rate : 1.0;
    const int to_held = static_cast<int>(std::ceil(host_scale * held_rate_scale - 1.0e-9));
    out.oversample = std::min(kMaxLoopOversample, std::max(out.oversample, to_held));
  }
  const double omega = constants::kTwoPiD / std::max(1.0f, out.achieved_period);
  // Past this the read has nothing left to repay and a boost would only amplify noise.
  constexpr double kMinMagnitude = 0.25;
  const double magnitude = std::max(kMinMagnitude, rt::lagrange3_magnitude(out.delay, omega));
  if (!(sample_rate > 0.0)) {
    out.interp_gain = static_cast<float>(std::max(1.0, 1.0 / magnitude));
  } else if (sample_rate != kLossVoicedSr) {
    const double voiced_period =
        static_cast<double>(out.achieved_period) * kLossVoicedSr / sample_rate;
    const double voiced_delay = std::max<double>(min_delay, voiced_period - comp_samples);
    const double voiced_magnitude = std::max(
        kMinMagnitude, rt::lagrange3_magnitude(voiced_delay, constants::kTwoPiD / voiced_period));
    out.interp_gain = static_cast<float>(std::max(1.0, voiced_magnitude / magnitude));
  }
  return out;
}

/// The budget of a voice with several delay lines in one loop: floored when any line is, and
/// asking for the largest factor any of them does. The remaining fields are @p primary's.
inline LoopBudget worst_loop_budget(LoopBudget primary, const LoopBudget& other) noexcept {
  primary.floored = primary.floored || other.floored;
  primary.oversample = std::max(primary.oversample, other.oversample);
  return primary;
}

/// The oversampling factor a voice runs its loop at. @p build(factor) configures the voice for a
/// loop running @p factor times per host sample (every coefficient, state and period built for
/// that internal rate, the host rate times @p factor) and returns the LoopBudget it ended up with;
/// it is called with 1 first, and the voice is left configured for the factor returned.
///
/// loop_budget()'s estimate only seeds the search: the compensation a loss pole adds is not
/// exactly proportional to the factor, so each candidate is read back from the voice itself and
/// raised by one until the period clears the floor or kMaxLoopOversample is reached.
template <typename Build>
inline int settle_loop_oversample(Build&& build) noexcept {
  LoopBudget budget = build(1);
  if (budget.oversample <= 1) return 1;
  int factor = std::clamp(budget.oversample, 2, kMaxLoopOversample);
  for (;;) {
    budget = build(factor);
    if (!budget.floored || factor >= kMaxLoopOversample) return factor;
    ++factor;
  }
}

/// Brings a loop run at an integer multiple of the host rate back to the host rate: the voice
/// renders @p factor internal samples per host sample and this keeps the last @p factor of them
/// through a linear-phase windowed-sinc lowpass at the host Nyquist before taking one.
///
/// A factor of 1 is not filtered at all (run() is the step itself), so a voice that never needed
/// the oversampling renders bit for bit what it did without it. Otherwise the output is delayed by
/// kHalfLengthPeriods host samples.
class LoopDecimator {
 public:
  /// Host samples of filter on each side of the centre tap.
  static constexpr int kHalfLengthPeriods = 8;
  static constexpr int kMaxTaps = 2 * kHalfLengthPeriods * kMaxLoopOversample + 1;

  /// Builds the filter for @p factor (clamped to 1..kMaxLoopOversample) and clears its history.
  void configure(int factor) noexcept {
    factor_ = std::clamp(factor, 1, kMaxLoopOversample);
    taps_ = 1;
    reset();
    if (factor_ == 1) return;
    taps_ = 2 * kHalfLengthPeriods * factor_ + 1;
    const int centre = kHalfLengthPeriods * factor_;
    double sum = 0.0;
    std::array<double, kMaxTaps> raw{};
    for (int j = 0; j < taps_; ++j) {
      const double x = static_cast<double>(j - centre) / factor_;
      const double sinc = x == 0.0 ? 1.0 : std::sin(constants::kPiD * x) / (constants::kPiD * x);
      const double hann = 0.5 - 0.5 * std::cos(constants::kTwoPiD * j / (taps_ - 1));
      raw[static_cast<size_t>(j)] = sinc * hann;
      sum += raw[static_cast<size_t>(j)];
    }
    for (int j = 0; j < taps_; ++j) {
      taps_table_[static_cast<size_t>(j)] = static_cast<float>(raw[static_cast<size_t>(j)] / sum);
    }
  }

  void reset() noexcept {
    history_.fill(0.0f);
    head_ = 0;
  }

  int factor() const noexcept { return factor_; }

  /// Calls @p step (one internal sample, returning its output) factor times and returns the
  /// decimated host sample.
  template <typename Step>
  float run(Step&& step) noexcept {
    if (factor_ == 1) return step();
    for (int i = 0; i < factor_; ++i) {
      history_[static_cast<size_t>(head_)] = step();
      head_ = head_ + 1 == taps_ ? 0 : head_ + 1;
    }
    // Newest sample sits just behind head_.
    double acc = 0.0;
    int idx = head_;
    for (int j = 0; j < taps_; ++j) {
      idx = idx == 0 ? taps_ - 1 : idx - 1;
      acc += static_cast<double>(taps_table_[static_cast<size_t>(j)]) *
             static_cast<double>(history_[static_cast<size_t>(idx)]);
    }
    return static_cast<float>(acc);
  }

 private:
  int factor_ = 1;
  int taps_ = 1;
  int head_ = 0;
  std::array<float, kMaxTaps> taps_table_{};
  std::array<float, kMaxTaps> history_{};
};

/// Raises @p gain by @p interp_gain without lifting the loop past what the sub-fundamental ring
/// bound allows, so the boost never turns DC into a runaway.
inline float repay_interpolation(float gain, float interp_gain) noexcept {
  const float ceiling = std::max(gain, sub_fundamental_gain_cap(gain));
  return std::min(gain * interp_gain, ceiling);
}

/// The factor by which a loop's per-traversal target gains grow so the Lagrange read's magnitude at
/// the fundamental is paid from the same budget. Taken BEFORE the loss filter is solved, so the
/// solver's sub-fundamental ring bound is sized for the boosted target and DC never rings longer
/// than that bound allows. @p pole_estimate is the loss pole the loop will carry, which sets the
/// delay left to the line.
inline float interpolation_repayment(float g_target, float period_samples,
                                     float pole_estimate) noexcept {
  const float comp =
      1.0f + onepole_group_delay_samples(pole_estimate, constants::kTwoPi / period_samples);
  const float repaid =
      repay_interpolation(g_target, loop_budget(period_samples, comp, 1.0f).interp_gain);
  return g_target > 0.0f ? repaid / g_target : 1.0f;
}

/// Highest flat loss gain a wind bore may reach once it repays the read: the in-loop highpass and
/// the read's own roll-off keep the sub-fundamental response under it, so the figure applies to
/// the fundamental, where the loop gain returns to the shipped one.
inline constexpr float kWindInterpLossCeil = 1.3f;

/// repay_interpolation() for a wind bore's flat loss gain, bounded by kWindInterpLossCeil.
inline float repay_wind_loss(float loss_gain, float interp_gain) noexcept {
  return std::min(loss_gain * interp_gain, std::max(loss_gain, kWindInterpLossCeil));
}

/// One string loop: a circular delay line read at a fractional offset and closed
/// through a one-pole loss filter and a per-traversal gain.
struct StringLoop {
  /// Delay line (a span of the voice's slab) and its length. The line spans the
  /// whole slab rather than the current note's period: the length is what bounds
  /// a downward bend, and the clamp enforcing it saturates silently — a glide
  /// stops descending while the note keeps sounding — so a per-note span is a
  /// pitch ceiling with nothing to hear it by.
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

  /// A second, cascaded loss pole (off by default: loop2_active == false skips
  /// it entirely, bit-identical to the single-pole form). Lifts the one-pole
  /// tilt ceiling by giving the loop a third, independent decay point.
  float alpha2 = 1.0f;
  float lp_state2 = 0.0f;
  bool loop2_active = false;

  /// Per-traversal amplitude factor for the sounding t60 (the product of both
  /// poles' front gains when the second is active), and the one release()
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
  ///
  /// @p has_pole2 engages the cascaded second pole (@p a2, @p g2); left false
  /// (the default) the loop is exactly the single-pole form above, bit-for-bit.
  void configure_filter(float* slab, int capacity, float period_samples, float a, float g,
                        float release_g, bool has_pole2 = false, float a2 = 0.0f,
                        float g2 = 1.0f) noexcept {
    active_ = false;
    buffer = slab;
    period = period_samples;
    write = 0;
    alpha = 1.0f - a;
    lp_state = 0.0f;
    float comp = onepole_group_delay_samples(a, constants::kTwoPi / period_samples);
    loop2_active = has_pole2;
    alpha2 = 1.0f - a2;
    lp_state2 = 0.0f;
    if (has_pole2) comp += onepole_group_delay_samples(a2, constants::kTwoPi / period_samples);
    loop_comp = 1.0f + comp;
    gain = g * g2;  // g2 == 1.0f by default, an exact IEEE754 no-op
    release_gain = release_g;
    size = capacity;
    if (buffer != nullptr) {
      std::fill(buffer, buffer + static_cast<size_t>(std::max(0, size)), 0.0f);
    }
    active_ = buffer != nullptr && size >= 5;
  }

  /// Leaves the loop silent and skipped: a call site gates on gain or on its own
  /// mix level, and a disengaged loop must not carry state from the last note.
  void disable() noexcept {
    active_ = false;
    size = 0;
    write = 0;
    lp_state = 0.0f;
    lp_state2 = 0.0f;
    loop2_active = false;
    gain = 0.0f;
  }

  /// Returns the clamped delay used by the travelling-wave read for @p ratio.
  /// The ratio is the per-sample pitch factor (bend / vibrato / tension), 1 =
  /// on pitch; it scales the frequency, so it divides the delay. Callers that
  /// need a second physical tap should use this same value so both taps obey
  /// loop compensation and the delay-line bounds.
  float effective_delay(float ratio) const noexcept {
    return std::clamp(period / ratio - loop_comp, 1.0f, static_cast<float>(size - 4));
  }

  /// Writes @p input into the line and reads the delayed sample back. The
  /// returned value is the string's output BEFORE the loss filter — shape it if
  /// the instrument shapes it, then hand it to commit().
  float advance(float input, float ratio) noexcept {
    if (!active_) return 0.0f;
    const float delay = effective_delay(ratio);
    const int delay_q8 = static_cast<int>(delay * 256.0f);
    return rt::lagrange3_fractional_delay(buffer, static_cast<size_t>(size), write, delay_q8,
                                          input);
  }

  /// Closes the loop: the (possibly shaped) delayed sample enters the loss
  /// filter, cascaded through the second pole when engaged.
  void commit(float shaped) noexcept {
    if (!active_) return;
    lp_state += alpha * (shaped - lp_state);
    if (loop2_active) lp_state2 += alpha2 * (lp_state - lp_state2);
  }

  /// advance() then commit(), for a loop with nothing shaped inside it.
  float process(float input, float ratio) noexcept {
    const float out = advance(input, ratio);
    commit(out);
    return out;
  }

  /// The feedback term to add into the next sample's loop input.
  float feedback() const noexcept {
    if (!active_) return 0.0f;
    return gain * (loop2_active ? lp_state2 : lp_state);
  }

  /// Note-off: re-target the decay to the damped t60. Never lengthens a decay
  /// that is already shorter than the damper's.
  void release() noexcept { gain = std::min(gain, release_gain); }

  /// Immediate silence.
  void kill() noexcept {
    active_ = false;
    gain = 0.0f;
    lp_state = 0.0f;
    lp_state2 = 0.0f;
  }

 private:
  bool active_ = false;
};

}  // namespace sonare::midi::synth
