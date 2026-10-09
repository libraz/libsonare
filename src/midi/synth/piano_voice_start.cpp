#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice.h"
#include "midi/synth/piano_voice_calibration.h"
#include "midi/synth/pitch.h"
// Only for solve_string_loop_filter(): this voice keeps its own loop (it has an
// in-loop dispersion cascade and a two-stage feedback the shared one does not
// model), but the loss filter it needs is the same one, solved the same way.
#include "midi/synth/string_loop.h"
#include "midi/synth/voice_random.h"
#include "rt/biquad_design.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

using namespace piano_detail;
using sonare::constants::kButterworthQ;
using sonare::constants::kPi;
using sonare::constants::kTwoPi;

namespace {
/// Floor on a one-pole coefficient; it must sit far below any requested corner
/// (0.076 Hz at 48 kHz) -- a floor of 0.01 was 76.8 Hz and pinned every note
/// below B2 to one corner, leaving the keytrack unfittable there.
constexpr float kOnePoleAlphaFloor = 1.0e-5f;
/// Bound on strike-point excursion as a fraction of this blow's peak felt
/// compression. A safety bound, not a voicing control, so not tunable.
constexpr float kYieldExcursionCap = 0.7f;
/// Section Qs of the fourth-order Butterworth radiation highpass,
/// 1/(2 cos(pi/8)) and 1/(2 cos(3pi/8)): fixed by the filter order, not voiced.
constexpr std::array<float, 2> kRadiationHpSectionQ = {0.54119610f, 1.30656296f};
}  // namespace

namespace piano_detail {

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s.
float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  return std::exp(-6.907755279f / loops_to_t60);
}

/// Exact phase delay (samples) of the first-order allpass
/// H(z) = (a + z^-1)/(1 + a z^-1) at normalized frequency @p w.
float allpass_phase_delay(float a, float w) noexcept {
  const float sinw = std::sin(w);
  const float cosw = std::cos(w);
  const float phi = std::atan2(-sinw, a + cosw) - std::atan2(-a * sinw, 1.0f + a * cosw);
  return -phi / std::max(w, 1.0e-6f);
}

/// Phase delay (samples) of the one-pole loop lowpass y = (1-a)x + a*y^-1 at
/// normalized frequency @p w.
float onepole_phase_delay(float a, float w) noexcept { return onepole_group_delay_samples(a, w); }

/// First-order allpass coefficient a (<= 0) for a cascade of @p stages that
/// disperses the waveguide loop into the stiff-string law f_n =
/// n*f0*sqrt(1 + B*n^2). The loop resonates where the total round-trip phase
/// delay equals an integer number of periods, and only the lowpass and the
/// allpass cascade vary the phase delay with frequency, so a is solved
/// (bisection) to supply the stiff-string phase-delay differential between
/// the fundamental and a high reference partial, then clamped so the
/// per-stage delay still fits the loop budget. Endpoint-matched after Rauhala
/// & Valimaki (2006); RT-safe (bounded, allocation-free, deterministic).
float dispersion_allpass_a(float b_coeff, float w0, float lp_a, int stages,
                           float phase_budget) noexcept {
  if (b_coeff <= 0.0f || stages <= 0) return 0.0f;
  // Reference partial: high enough for a measurable differential but shrunk
  // until its stiff-string frequency sits safely below Nyquist (so the
  // treble, where B is large, still gets dispersion instead of bailing out).
  const float n_max = 0.8f * kPi / std::max(w0, 1.0e-6f);
  int n_ref = std::clamp(static_cast<int>(n_max), 2, 12);
  while (n_ref > 2 && w0 * static_cast<float>(n_ref) *
                              std::sqrt(1.0f + b_coeff * static_cast<float>(n_ref) *
                                                   static_cast<float>(n_ref)) >=
                          0.9f * kPi)
    --n_ref;
  const float fr = static_cast<float>(n_ref);
  const float w1 = w0 * std::sqrt(1.0f + b_coeff);
  const float wr = w0 * fr * std::sqrt(1.0f + b_coeff * fr * fr);
  if (wr >= 0.97f * kPi) return 0.0f;
  const float period = kTwoPi / w0;
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

/// Per-loop gain for a damper resting partially on the string: the decay time
/// t60 ~ -K/ln(gain) is interpolated geometrically between the natural ring
/// @p natural and the full-damper gain @p damped as the contact @p strength
/// goes 0 -> 1, so a light touch slows the ring gently while firm contact
/// reaches the full damp.
float partial_damp_gain(float natural, float damped, float strength) noexcept {
  const float ln_nat = std::log(std::max(natural, 1.0e-6f));
  const float ln_dmp = std::log(std::max(damped, 1.0e-6f));
  if (ln_nat >= -1.0e-7f) return damped;  // no natural decay: jump to the damp
  const float a = std::log(-ln_nat);
  const float b = std::log(-ln_dmp);
  return std::exp(-std::exp(a + strength * (b - a)));
}

}  // namespace piano_detail

void PianoVoiceCore::start(const PianoPatchParams& params, double sample_rate, uint8_t note,
                           Velocity16 velocity, uint64_t seed, bool una_corda) noexcept {
  const double host_sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Strings too short to hold their pitch and decay at the host rate run at a multiple of it;
  // everything below is built for that internal rate.
  const int factor = settle_loop_oversample(
      [&](int f) { return configure(params, host_sr * f, note, velocity, seed, una_corda); });
  decimator_.configure(factor);
}

LoopBudget PianoVoiceCore::configure(const PianoPatchParams& params, double sr, uint8_t note,
                                     Velocity16 velocity, uint64_t seed, bool una_corda) noexcept {
  // Stretch tuning widens the octaves so the inharmonic partials lock the way
  // a tuned grand's do (sharp treble, flat bass; A4 anchored).
  const float f0 = note_to_hz(note) * std::exp2(piano_stretch_cents(note) / 1200.0f);
  const float period = static_cast<float>(sr) / f0;
  const float w0 = kTwoPi / period;
  VoiceRandomSequence jitter(seed);

  // Two-stage decay rates (stretched down the keyboard). The treble taper
  // shortens only the aftersound; the prompt stage instead blends toward the
  // aftersound rate away from the mid-range (see kTwoStageWidthOct).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  // The stretch lengthens the bass and does NOTHING above A4. Read as a signed
  // exponent it also compressed the treble, and that half of it is not in the
  // instrument: measured on three concert grands the aftersound t60 has no
  // trend from A0 to note 90, so there is nothing above A4 for a compression to
  // describe. It stacked with the taper below, and the two together took the
  // top of the keyboard to a fraction of the measured aftersound — a note
  // arriving as a click. Held at unity the taper alone sets the treble.
  const float octaves_below_a4 = std::max(0.0f, (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f);
  const float bass_scale = std::exp2(stretch * octaves_below_a4);
  // The aftersound taper rides its own octave axis, not the shared
  // `octaves_above_c4`: that one is capped where the loop DARKENING stops
  // steepening, and the aftersound knee sits an octave above it. Ordered through
  // std::max so a swept knee above the floor cannot invert the clamp.
  const float decay_knee_oct = kTrebleDecayKneeOct;
  const float decay_floor_oct = std::max(decay_knee_oct, kTrebleDecayFloorOct);
  const float decay_taper_oct = std::clamp((static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f,
                                           decay_knee_oct, decay_floor_oct) -
                                decay_knee_oct;
  const float slow_scale = bass_scale * std::exp2(-kTrebleDecayOct * decay_taper_oct);
  const float t60_slow =
      std::max(0.05f, std::max(params.decay_fast_s, params.decay_slow_s) * slow_scale);

  // Loop lowpass (frequency-dependent damping), closing toward the treble.
  const float octaves_above_c4 = std::min(
      std::max(0.0f, (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f), kTrebleTaperOctCap);
  // ...and closes into the bass as well: the wound strings' winding friction
  // damps the mid partials far faster than the plain-wire loop loss suggests.
  // Left open, the bass h4-h6 ring 3-4x longer than the reference — a bright
  // partial stack singing over the fundamental is a harpsichord register.
  const float octaves_below_c4 =
      std::max(0.0f, -(static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f);
  const float bright_eff =
      std::clamp(std::clamp(params.brightness, 0.0f, 1.0f) -
                     kTrebleBrightPerOct * octaves_above_c4 - kBassDarkPerOct * octaves_below_c4,
                 0.05f, 1.0f);
  // The loop lowpass runs once per round trip, so a fixed coefficient costs a
  // given absolute frequency a fixed number of dB PER TRAVERSAL -- and a note
  // an octave up makes twice as many traversals per second. The damping a
  // listener actually hears is therefore proportional to f0, which is not how
  // a string behaves: its losses belong to the wire and the air around it, not
  // to how often the wave happens to come round. Uncorrected, C6's second
  // partial falls 60 dB inside the first second and the treble renders as a
  // sine. Scaling the coefficient back by the traversal rate removes the
  // register dependence; the exponent sets how much of it is removed, and the
  // reference frequency is the note left untouched.
  const float rate_norm =
      std::pow(kLoopDampRefHz / std::max(f0, 1.0f), std::clamp(kLoopDampRateNorm, 0.0f, 1.0f));
  float lp_a =
      loss_pole_at_rate(std::clamp((1.0f - bright_eff) * 0.6f * rate_norm, 0.0f, 0.95f), sr);
  // Solve the pole from what the string has to DO instead of from a tone knob.
  //
  // The rate normalization above takes the register dependence out of the
  // coefficient, but the coefficient is still chosen for tone and its loss at
  // the upper partials is then whatever it happens to be — and it is applied
  // once per traversal, so a note making four thousand of them a second pays it
  // four thousand times. Measured at C8 the second partial falls 103 dB/s
  // faster than the fundamental where the reference holds the two within a few
  // decibels of each other for the whole note, and every voicing knob in the
  // engine moves that by under twenty. It is not a setting that is wrong, it is
  // a quantity nothing states.
  //
  // Stating it is a two-point filter design: the fundamental keeps the gain its
  // t60 asks for, and the partial at a FIXED quoted frequency keeps the smaller
  // one its own target asks for. Fixed rather than at some multiple of f0,
  // because string damping is measured against frequency and because a target
  // quoted at the octave is two points a fraction of a percent apart in the
  // bass — indistinguishable to one pole — while being a brick wall four
  // octaves up. Same solver the plucked cores use (string_loop.h), which found
  // the identical defect from the other end.
  if (kLoopSolveHf > 0.0f) {
    const float quote_w = kTwoPi *
                          std::clamp(kLoopHfQuoteHz, 200.0f, 0.45f * static_cast<float>(sr)) /
                          static_cast<float>(sr);
    // Above the quote frequency there is no tilt left to ask for: the note's
    // own fundamental is already past the point the target describes.
    if (quote_w > w0 * 1.5f) {
      const float ratio = std::max(1.0f, kLoopHfDecayRatio);
      const float g0 = loop_gain_for(period, sr, t60_slow);
      const float g_ref = loop_gain_for(period, sr, t60_slow / ratio);
      lp_a = std::clamp(solve_string_loop_filter(w0, quote_w, g0, g_ref).a, 0.0f, 0.95f);
    }
  }
  loop_alpha_ = 1.0f - lp_a;
  const float tau_lp = onepole_phase_delay(lp_a, w0);

  // Stiffness dispersion: the per-note inharmonicity coefficient B drives a
  // first-order allpass cascade that stretches the partials sharp to
  // f_n = n*f0*sqrt(1 + B*n^2). The patch dispersion knob scales B
  // (0 = harmonic string).
  // Faded out where the cascade no longer fits the loop (see
  // kDispersionFadeNoteLo). Smoothstepped, so the top octave loses its stretch
  // gradually rather than at one note boundary.
  const float fade_lo = kDispersionFadeNoteLo;
  const float fade_hi = std::max(fade_lo + 1.0f, kDispersionFadeNoteHi);
  const float fade_x =
      std::clamp((static_cast<float>(note & 0x7Fu) - fade_lo) / (fade_hi - fade_lo), 0.0f, 1.0f);
  const float dispersion =
      std::clamp(params.dispersion, 0.0f, 1.0f) * (1.0f - fade_x * fade_x * (3.0f - 2.0f * fade_x));
  const float b_coeff = piano_inharmonicity_b(note) * dispersion;
  const float phase_budget = period - 4.0f - tau_lp;
  const float ap_a = dispersion_allpass_a(b_coeff, w0, lp_a, kPianoDispersionStages, phase_budget);

  const float oct_from_c4_signed = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contrast_x = oct_from_c4_signed - kTwoStageCenterOct;
  const float contrast =
      std::exp(-(contrast_x * contrast_x) / (kTwoStageWidthOct * kTwoStageWidthOct));
  const float inv_fast_full = 1.0f / std::max(0.05f, params.decay_fast_s * bass_scale);
  const float inv_slow = 1.0f / t60_slow;
  const float t60_fast =
      std::min(t60_slow, 1.0f / (inv_slow + contrast * std::max(0.0f, inv_fast_full - inv_slow)));

  // The patch string count is the treble voicing; the real grand strings the
  // bass with fewer (a single wound string has no unison aftersound).
  num_strings_ =
      std::clamp(std::min(params.strings, piano_unison_strings(note)), 1, kMaxPianoStrings);
  const float spread = std::max(0.0f, params.detune_cents);
  // Uneven strike energy across the unison (seeded ramp, mean-normalized so
  // the note level is independent of the string count).
  std::array<float, kMaxPianoStrings> strike_w{};
  float strike_mean = 0.0f;
  for (int i = 0; i < num_strings_; ++i) {
    float w = 1.0f;
    if (num_strings_ > 1) {
      w -= kUnisonStrikeUneven * static_cast<float>(i) / static_cast<float>(num_strings_ - 1);
      w *= 1.0f + 0.1f * jitter.bipolar_at(static_cast<uint64_t>(16 + i));
    }
    strike_w[static_cast<size_t>(i)] = std::max(w, 0.1f);
    strike_mean += strike_w[static_cast<size_t>(i)];
  }
  strike_mean /= static_cast<float>(num_strings_);
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.buffer = slab_ != nullptr ? slab_ + static_cast<size_t>(i) * string_capacity_ : nullptr;
    // Micro-detune: symmetric spread plus seeded jitter.
    float offset = 0.0f;
    if (num_strings_ > 1) {
      offset = spread * (static_cast<float>(i) / static_cast<float>(num_strings_ - 1) - 0.5f);
      offset *= 1.0f + 0.2f * jitter.bipolar_at(static_cast<uint64_t>(i));
    }
    const float detune_ratio = std::exp2(offset / 1200.0f);
    s.base_period = period / detune_ratio;
    s.strike_weight = strike_w[static_cast<size_t>(i)] / strike_mean;
    // Uneven bridge coupling (seeded ramp, mean 1 so the note level is
    // independent of the string count): lets the antisymmetric (aftersound)
    // mode radiate at the weight-difference level.
    s.radiate_weight = 1.0f;
    if (num_strings_ > 1) {
      s.radiate_weight += kUnisonRadSpread *
                          (static_cast<float>(i) / static_cast<float>(num_strings_ - 1) - 0.5f) *
                          (1.0f + 0.3f * jitter.bipolar_at(static_cast<uint64_t>(40 + i)));
    }
    // Per-string stiffness spread: decoheres the partial-by-partial unison
    // beat rates (the compensation below keeps the fundamental tuning exact).
    s.ap_a = std::clamp(
        ap_a * (1.0f + kUnisonStiffJitter * jitter.bipolar_at(static_cast<uint64_t>(24 + i))),
        -0.998f, 0.0f);
    s.ap_state.fill(0.0f);
    s.lp_state = 0.0f;
    s.write_index = 0;
    const float tau_ap = allpass_phase_delay(s.ap_a, w0);
    s.comp = 1.0f + tau_lp + static_cast<float>(kPianoDispersionStages) * tau_ap;
    // Compensate the loop lowpass's own loss at the fundamental so the patch
    // t60s stay the FUNDAMENTAL's decay; the darkened loop then only shortens
    // the upper partials (never pushed to/over unity: the LP loss is real).
    const float lp_h1_gain =
        (1.0f - lp_a) /
        std::sqrt(std::max(1.0e-9f, 1.0f - 2.0f * lp_a * std::cos(w0) + lp_a * lp_a));
    const float lp_comp = std::min(1.0f / std::max(1.0e-3f, lp_h1_gain), 1.0f / 0.9f);
    s.g_slow = std::min(0.99997f, loop_gain_for(s.base_period, sr, t60_slow) * lp_comp);
    s.g_fast = std::min(s.g_slow, loop_gain_for(s.base_period, sr, t60_fast) * lp_comp);
    s.g_slow_natural = s.g_slow;
    s.g_fast_natural = s.g_fast;
    // The line spans the whole slab rather than this note's period: the line
    // length is what bounds a downward bend, and the clamp that enforces it
    // saturates silently rather than breaking.
    if (s.buffer != nullptr && string_capacity_ > 0) {
      std::fill(s.buffer, s.buffer + static_cast<size_t>(string_capacity_), 0.0f);
    }
  }
  for (int i = num_strings_; i < kMaxPianoStrings; ++i) strings_[static_cast<size_t>(i)] = String{};
  bridge_ = 0.0f;
  bridge_drain_ = 0.0f;
  // Corner of the drain's band limit, as a multiple of this note's own
  // fundamental so the mechanism keeps the same reach in partials at every
  // pitch. Zero, or a corner at or above Nyquist, leaves the coefficient at 1,
  // where the filter is the identity and the drain is exactly what it was.
  const float drain_partials = std::max(0.0f, kTwoStageDrainPartials);
  const float drain_corner = drain_partials * f0;
  drain_lp_a_ = (drain_partials <= 0.0f || drain_corner >= 0.5f * static_cast<float>(sr))
                    ? 1.0f
                    : 1.0f - std::exp(-kTwoPi * drain_corner / static_cast<float>(sr));
  // Upper band: a fixed corner, so the drain is a shelf in absolute frequency
  // rather than in this note's partials, and a weight divided by the note's own
  // fundamental so that what it costs per SECOND is the same at every pitch.
  const float hf_corner = std::clamp(kBridgeHfHz, 20.0f, 0.45f * static_cast<float>(sr));
  drain_hf_a_ = 1.0f - std::exp(-kTwoPi * hf_corner / static_cast<float>(sr));
  drain_hi_w_ =
      std::max(0.0f, kBridgeHfDrain) * std::max(kBridgeHfRefHz, 1.0f) / std::max(f0, 1.0f);
  if (drain_hi_w_ > 0.0f) {
    // Bound it where the quantity has meaning: the drain is a multiple of the
    // gap between the two loop gains, so it is that gap, not the weight, that
    // says how close a traversal comes to removing the coherent component
    // outright. Taken over the widest string so no member of the unison can
    // exceed the ceiling on its own.
    float widest = 0.0f;
    for (int i = 0; i < num_strings_; ++i) {
      const String& s = strings_[static_cast<size_t>(i)];
      widest = std::max(widest, s.g_slow - s.g_fast);
    }
    if (widest > 1.0e-9f) {
      drain_hi_w_ =
          std::min(drain_hi_w_, std::max(0.0f, kBridgeHfDrainMax) * strings_[0].g_slow / widest);
    }
  }
  bridge_hf_lp_ = 0.0f;
  drain_out_ = 0.0f;
  // Damper keytrack: slightly heavier felt on the wound bass strings, and a
  // SHORTER stop toward the treble — the treble string carries so little
  // energy that even its light damper chokes it almost immediately (reference
  // pianos stop a C5 in ~200 ms where a C4 rings ~400 ms into the felt). Flat
  // across the tenor/alto anchor (C3-C4) where the felt geometry sits closest
  // to nominal, then bends smoothly (zero slope at the anchor edges, so no
  // audible register step) into the bass and treble.
  constexpr float kAnchorLowNote = 48.0f;   // C3
  constexpr float kAnchorHighNote = 60.0f;  // C4
  constexpr float kBassSpan = 20.0f;
  constexpr float kTrebleSpan = 10.0f;
  constexpr float kBassGain = 0.4f;
  constexpr float kTrebleGain = -0.45f;
  const float note_f = static_cast<float>(note & 0x7Fu);
  float damper_keytrack = 1.0f;
  if (note_f < kAnchorLowNote) {
    const float x = std::clamp((kAnchorLowNote - note_f) / kBassSpan, 0.0f, 1.0f);
    damper_keytrack += kBassGain * x * x * (3.0f - 2.0f * x);
  } else if (note_f > kAnchorHighNote) {
    const float x = std::clamp((note_f - kAnchorHighNote) / kTrebleSpan, 0.0f, 1.0f);
    damper_keytrack += kTrebleGain * x * x * (3.0f - 2.0f * x);
  }
  // Felt loss falls off with how hard the string drives it, so a soft note is
  // damped softly (see kDamperVelSlope).
  const float damper_vel_scale =
      std::min(kDamperVelScaleMax, std::exp(kDamperVelSlope * (kDamperVelAnchor - velocity.f7())));
  const float release_t60 =
      std::max(0.01f, params.release_damp_s * damper_keytrack * damper_vel_scale);
  release_gain_ = loop_gain_for(period, sr, release_t60);

  // Modal top-octave bank (see piano_voice.h). Built from the string set the
  // loop above already derived -- same periods, same detune, same strike and
  // radiation weights -- so the two paths describe one instrument and the
  // crossover has nothing to reconcile but the synthesis method.
  const float cross_lo = kModalCrossNoteLo;
  const float cross_hi = std::max(cross_lo, kModalCrossNoteHi);
  const float cross_x = cross_hi > cross_lo
                            ? std::clamp((note_f - cross_lo) / (cross_hi - cross_lo), 0.0f, 1.0f)
                            : (note_f >= cross_lo ? 1.0f : 0.0f);
  modal_mix_ = cross_x * cross_x * (3.0f - 2.0f * cross_x);
  modal_env_ = 1.0f;
  modal_damp_ = 1.0f;
  modal_release_ = std::exp(-6.907755279f / (static_cast<float>(sr) * release_t60));
  // The prompt stage, at the rate the loop's own two t60s already imply. Their
  // reciprocals differ by exactly the extra damping the fast stage applies, so
  // the envelope's time constant falls out of the pair with nothing fitted: at
  // zero contrast t60_fast IS t60_slow, the difference is zero, and the
  // envelope is the constant 1 -- the bank as it was, at any residue.
  modal_prompt_ = 1.0f;
  modal_residue_ = std::clamp(kModalResidue, 0.0f, 1.0f);
  const float prompt_rate = std::max(0.0f, 1.0f / t60_fast - 1.0f / t60_slow);
  modal_prompt_r_ =
      prompt_rate > 0.0f ? std::exp(-6.907755279f * prompt_rate / static_cast<float>(sr)) : 1.0f;
  num_modal_ = 0;
  modal_active_count_ = 0;
  modal_cached_ratio_ = 1.0f;
  if (modal_mix_ > 0.0f) {
    // Full stiffness here, not the faded `dispersion` the loop uses: that fade
    // exists because the allpass cascade runs out of loop to sit in, and this
    // path has no loop. The top octave is where B is largest, so it is also
    // where the fade cost the most.
    const float modal_b = piano_inharmonicity_b(note) * std::clamp(params.dispersion, 0.0f, 1.0f);
    const float damp_pow = std::max(0.0f, kModalDampPow);
    // The stiff-string law is quoted against the IDEAL string's f0, so its own
    // first partial already sits sqrt(1+B) sharp -- seven cents at C7. The loop
    // does not carry that offset: its delay compensation is taken at w0, which
    // pins the fundamental to the tuned pitch and lets the stretch accumulate
    // upward from there. Same convention here, or the top octave would arrive
    // sharp of the tuning the Railsback curve just placed it at.
    const float modal_f1 = std::sqrt(1.0f + modal_b);
    for (int i = 0; i < num_strings_ && num_modal_ < kModalModes; ++i) {
      const String& s = strings_[static_cast<size_t>(i)];
      if (s.base_period <= 1.0f) continue;
      const float sf0 = static_cast<float>(sr) / s.base_period;
      const float weight = kModalLevel * s.strike_weight * s.radiate_weight;
      for (int n = 1; n <= kModalPartials && num_modal_ < kModalModes; ++n) {
        const float fn = static_cast<float>(n) * sf0 *
                         std::sqrt(1.0f + modal_b * static_cast<float>(n) * static_cast<float>(n)) /
                         modal_f1;
        const float w = kTwoPi * fn / static_cast<float>(sr);
        const float t60_n =
            std::max(0.01f, t60_slow * std::pow(fn / std::max(sf0, 1.0f), -damp_pow));
        const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * t60_n));
        ModalMode& m = modal_[static_cast<size_t>(num_modal_++)];
        m.omega0 = w;
        m.r = r;
        m.weight = weight;
        m.base_period = s.base_period;
        m.y1 = 0.0f;
        m.y2 = 0.0f;
        if (w >= 0.9f * kPi) {
          m.audible = false;
          m.a1 = 0.0f;
          m.a2 = 0.0f;
          m.gain = 0.0f;
        } else {
          // Ratio-one operation order; render() refreshes on a live pitch change.
          m.audible = true;
          m.a1 = 2.0f * r * std::cos(w);
          m.a2 = -r * r;
          m.gain = weight * 2.0f * std::sin(w) / s.base_period;
          modal_active_indices_[modal_active_count_++] = static_cast<uint8_t>(num_modal_ - 1);
        }
      }
    }
  }

  // Dynamic felt hammer (F = k * x^p with hysteretic loss), integrated per
  // sample against the string at the strike point. The felt stiffness k is
  // calibrated so a mezzo-forte blow lands the reference contact time for the
  // register; from there the Hertz velocity laws (harder+shorter with faster
  // blows), the treble's long full-period dwell and the bass re-contact
  // chatter all EMERGE from the interaction instead of being prescribed.
  const float vel01 = std::max(velocity.f7() / 127.0f, 0.02f);
  const float p = std::clamp(params.hammer_exponent, 1.5f, 4.0f);
  const float amp_exp = 2.0f * p / (p + 1.0f);
  const float dyn =
      std::clamp(params.hammer_dynamics, 0.0f, 1.0f) * (una_corda ? kUnaCordaDynScale : 1.0f);
  // Reference contact time for the register (mf): the patch contact scaled by
  // register, floored in fundamental PERIODS (treble dwell ~ a full period).
  float contact_ms = std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                     std::exp2(-(static_cast<float>(note & 0x7Fu) - 69.0f) /
                               std::max(1.0f, kContactKeytrackSemis));
  const float octaves_from_c4 = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contact_floor_periods = std::clamp(
      kContactPeriodsAtC4 + kContactPeriodsPerOct * octaves_from_c4, 0.0f, kContactPeriodsMax);
  contact_ms =
      std::max(contact_ms, contact_floor_periods * 1000.0f * period / static_cast<float>(sr));
  const float tau_mf = std::max(8.0f, contact_ms * 0.001f * static_cast<float>(sr));
  // Free bounce of a unit mass on F = k * x^p from unit velocity lasts
  // c(p) * k^(-1/(p+1)) samples; c(p) fitted over p in [1.5, 4].
  const float c_p = 3.28f - 0.066f * p;
  // Felt hysteresis: loading is stiffer than unloading, which skews the force
  // pulse forward and bleeds energy so the hammer leaves the string cleanly.
  ham_p_ = p;
  ham_mu_ = kHammerHysteresis;
  ham_y_ = 0.0f;
  // Hammer speed normalized at the mezzo-forte reference; hammer_dynamics
  // widens the pp<->ff speed spread around that pivot.
  ham_v_ = std::pow(vel01 / kHammerMfVel, 1.0f + 0.6f * dyn);
  ham_on_ = true;
  ham_ttl_ = static_cast<int>(3.0f * tau_mf);  // shank check truncates a riding hammer
  // Calibrated mezzo-forte felt stiffness and the bounce it makes (unit mass):
  // peak compression x_max = ((p+1)/2k)^(1/(p+1)) and peak force k*x_max^p.
  // Both are the reference the injection normalizes against, so the velocity
  // LEVEL curve (~ v^(2p/(p+1))) comes out of the dynamics rather than out of
  // the normalization -- anything blow-dependent folded into the stiffness has
  // to stay out of these two or it divides that curve straight back out.
  // The una-corda felt patch is softer (lower k -> longer, darker contact).
  ham_k_ = std::pow(c_p / tau_mf, p + 1.0f) * (una_corda ? 0.5f : 1.0f);
  const float x_max_mf = std::pow(0.5f * (p + 1.0f) / ham_k_, 1.0f / (p + 1.0f));
  const float f_peak_mf = ham_k_ * std::pow(x_max_mf, p);
  // This blow's free-bounce contact, and the ceiling the string's reflection
  // puts on it (see kContactPeriodsPerBlowMax). Duration goes as k^(-1/(p+1)),
  // so holding it to the ceiling costs that ratio raised to p+1 in stiffness.
  // Only ham_k_ moves: the two mezzo-forte references above are the injection's
  // normalization and have to stay where they are.
  const float tau_blow = tau_mf * std::pow(std::max(ham_v_, 1.0e-4f), -(p - 1.0f) / (p + 1.0f));
  const float dwell_cap = std::max(kContactPeriodsPerBlowMax, contact_floor_periods) * period;
  ham_k_ *= std::pow(std::max(1.0f, tau_blow / std::max(dwell_cap, 1.0e-6f)), p + 1.0f);
  // Level reference: velocity-scaled for the noise/knock paths (as before);
  // the injection normalizes to the MF level so the velocity LEVEL curve
  // (~ v^(2p/(p+1))) comes out of the dynamics, not out of this constant.
  hammer_amp_ = kOutputLevel * std::pow(vel01, amp_exp) * (una_corda ? 0.8f : 1.0f);
  const float mf_level = kOutputLevel * std::pow(kHammerMfVel, amp_exp) * (una_corda ? 0.8f : 1.0f);
  ham_force_norm_ = f_peak_mf > 1.0e-12f ? mf_level / f_peak_mf : 0.0f;
  // Re-anchor from the peak onto the delivered momentum (see kInjImpulseNorm).
  // The peak-anchored norm goes as tau_mf, so dividing it by this note's contact
  // measured against C4's takes that dependence out and leaves the injection
  // carrying the same impulse in every register. Computed from the same two
  // rules the contact above uses rather than remembered, so a change to either
  // moves the anchor with it.
  const float impulse_norm = std::clamp(kInjImpulseNorm, 0.0f, 1.0f);
  if (impulse_norm > 0.0f) {
    const float period_c4 = static_cast<float>(sr) / note_to_hz(60.0f);
    const float contact_c4_ms =
        std::max(std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                     std::exp2(9.0f / std::max(1.0f, kContactKeytrackSemis)),
                 std::clamp(kContactPeriodsAtC4, 0.0f, kContactPeriodsMax) * 1000.0f * period_c4 /
                     static_cast<float>(sr));
    const float tau_c4 = std::max(8.0f, contact_c4_ms * 0.001f * static_cast<float>(sr));
    ham_force_norm_ *= std::pow(tau_c4 / tau_mf, impulse_norm);
  }
  const float tilt_span = std::max(0.0f, kInjTiltOctSpan);
  ham_force_norm_ *=
      std::exp2(kInjTiltDbOct * std::clamp(octaves_from_c4, -tilt_span, tilt_span) / 6.0206f);
  // String yield under the blow: the strike point recedes with a velocity
  // proportional to the net force through the string's wave admittance, and
  // the inverted reflection from the NEAR end (agraffe side, back after
  // 2 * strike_position * period = comb_delay_ samples) cancels that recess
  // — the felt is recompressed and the measured multi-hump piano force
  // curve (and the treble's full-period dwell) emerges.
  const float yield_kt =
      kStringYield * std::exp2(-kYieldTrebleOct * std::max(0.0f, octaves_from_c4));
  // The admittance is the STRING's (force in, transverse velocity out), so it
  // does not depend on the blow. The excursion the strike point can reach does:
  // it follows this blow's peak felt compression, which is read off the
  // stiffness the blow actually ran on rather than the mezzo-forte one, so a
  // blow held to the dwell ceiling compresses the felt less and drives the
  // string aside less in the same proportion. Pinning the excursion to the mf value
  // instead makes the string effectively RIGID above mezzo-forte, so the felt
  // absorbs the whole of a fortissimo blow and the contact shortens even
  // further than the free-bounce law alone would give.
  const float x_max_unit = std::pow(0.5f * (p + 1.0f) / ham_k_, 1.0f / (p + 1.0f));
  const float x_max_v = x_max_unit * std::pow(std::max(ham_v_, 1.0e-4f), 2.0f / (p + 1.0f));
  ys_adm_ = 0.5f * yield_kt * x_max_mf;
  ys_limit_ = kYieldExcursionCap * x_max_v;
  ys_ = 0.0f;
  last_force_ = 0.0f;
  ham_exit_ = -x_max_v;
  // The strike point moves out toward 1/8 of the speaking length on the bass
  // strings (mid/treble sits nearer 1/12): the 1/8 node notches h8 right
  // below the bridge-hill crown — the reference bass ladder's signature dip.
  const float strike_pos = std::clamp(params.strike_position, 0.0f, 0.5f) *
                           std::exp2(kStrikePosBassOct * std::max(0.0f, -octaves_from_c4));
  comb_delay_ = static_cast<int>(std::min(strike_pos, 0.5f) * period + 0.5f);
  comb_delay_ = std::min(comb_delay_, kHammerCombCapacity - 1);
  comb_idx_ = 0;
  comb_tail_ = 0;
  comb_hist_.fill(0.0f);
  noise_hist_.fill(0.0f);
  // Felt stiffening: compressed felt (hard strike) passes far more of the
  // pulse's top end — a velocity-driven one-pole on the injected force. The
  // dynamics-gated brightening also scales the footprint cap: compression
  // flattens the felt crown, so a hard strike's effective contact patch
  // passes higher partials (without this the width cap, which sits below the
  // stiffness cutoff at normal velocities, swallows the whole pp<->ff
  // brightness spread).
  const float dyn_bright = std::exp2(kHammerDynBrightOct * dyn * (vel01 - kHammerMfVel));
  const float exc_cutoff = kFeltCutoffContactCycles / std::max(1.0e-4f, contact_ms * 0.001f) *
                           std::exp2(kFeltCutoffVelOct * vel01) * dyn_bright *
                           (una_corda ? 0.4f : 1.0f);
  const float width_harm =
      kHammerWidthHarmonics * std::exp2(kWidthBassOct * std::max(0.0f, -octaves_from_c4) +
                                        kWidthTrebleOct * std::max(0.0f, octaves_from_c4));
  const float width_cutoff = std::min(exc_cutoff, width_harm * f0 * dyn_bright);
  exc_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * width_cutoff / static_cast<float>(sr)),
                          kOnePoleAlphaFloor, 1.0f);
  // The noise cutoff keytracks DOWN into the bass: the bass hammer is a
  // massive deep-felt head whose scrub spectrum is far darker than the small
  // hard treble hammer's — without this the bass attack carries the same
  // 2 kHz-wide burst as the treble and reads as a jack click.
  const float noise_cutoff = kStrikeNoiseCutoffScale * exc_cutoff *
                             std::exp2(-kNoiseCutoffBassOct * std::max(0.0f, -octaves_from_c4));
  noise_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * noise_cutoff / static_cast<float>(sr)),
                            kOnePoleAlphaFloor, 1.0f);
  noise_alpha3_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kNoiseSteepRatio * noise_cutoff / static_cast<float>(sr)),
      kOnePoleAlphaFloor, 1.0f);
  exc_lp_ = 0.0f;
  exc_lp2_ = 0.0f;
  body_lp_ = 0.0f;
  body_lp2_ = 0.0f;

  // Felt impact noise: a short broadband burst radiated with the knock (the
  // soft una-corda felt lands with far less impact noise than the grooved
  // normale surface).
  // The impact noise is part of the same blow — it rides the injection tilt
  // and the dynamics-gated felt compression (a compressed crown scrubs
  // louder, a soft blow on open felt barely rustles).
  // Scales the direct tap and the string-injected scrub alike, both read off it.
  noise_env_ = kStrikeNoiseGain * hammer_amp_ * dyn_bright * (una_corda ? 0.35f : 1.0f) *
               std::exp2(-kNoiseTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                         kInjTiltDbOct * std::clamp(octaves_from_c4, -1.25f, 1.25f) / 6.0206f) *
               noise_gain_at_rate(sr);
  knock_gain_ = kKnockGain * std::pow(std::max(vel01, 1.0e-4f), kKnockVelExp) *
                std::exp2(kKnockBassBoostOct * std::max(0.0f, -octaves_from_c4) -
                          kKnockTrebleTaperOct * std::max(0.0f, octaves_from_c4));
  // The same blow, told to the structure instead of to the listener. It carries
  // the knock's whole velocity law -- the blow itself, which the knock gets
  // through `thud_in`, times the extra exponent above -- and NONE of the
  // register grading, because the two describe different things: the gradings
  // say how the strike's radiated spectrum changes with pitch, which the felt
  // and the contact time do shape, while what the plate and the rim receive is
  // a force impulse whose size is the blow. Handed to the shared board rather
  // than mixed into this voice's output, so it reaches the case network without
  // passing the bridge coupling the sustained signal does.
  //
  // Quoted against mezzo-forte, like the injection's own normalization, so the
  // number is a size at the reference blow rather than a raw amplitude.
  const float blow_norm = mf_level > 0.0f ? hammer_amp_ / mf_level : 0.0f;
  const float blow_vel = std::pow(std::max(vel01, 1.0e-4f), kKnockVelExp);
  case_strike_ = kCaseStrikeGain * blow_norm * blow_vel;
  board_strike_ = kBoardStrikeGain * blow_norm * blow_vel *
                  std::exp2(kBoardStrikeTrebleOct * std::max(0.0f, octaves_from_c4));
  knock_lp_ = 0.0f;
  knock_lp2_ = 0.0f;
  knock_lp3_ = 0.0f;
  // The thud deepens and slows into the bass: the massive bass hammer rocks
  // the whole board, a boom that takes ~10 ms to develop — an instantaneous
  // bass onset is a jack pluck, not a hammer landing.
  const float thud_hz =
      kKnockThudHz * std::exp2(-kKnockThudBassOct * std::max(0.0f, -octaves_from_c4));
  knock_lp_a_ = std::clamp(1.0f - std::exp(-kTwoPi * thud_hz / static_cast<float>(sr)), 0.0f, 1.0f);
  knock_lp3_a_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kNoiseSteepRatio * thud_hz / static_cast<float>(sr)), 0.0f, 1.0f);
  bloom_ = 0.0f;
  const float bloom_tau_s = kBloomTauMsC4 * 0.001f * std::exp2(-kBloomTauOct * octaves_from_c4);
  bloom_a_ =
      std::clamp(1.0f - std::exp(-1.0f / (bloom_tau_s * static_cast<float>(sr))), 1.0e-4f, 1.0f);
  // Longitudinal mode bank. The input carries a DC/Nyquist zero (see render),
  // so each mode is exactly peak-normalized off the bandpass residue the way
  // the soundboard's are: two-pole modes taken raw pile their low-frequency
  // skirts up in phase, which here would put the squared drive's DC straight
  // back into the bass the bank exists to clear.
  long_level_ = kLongLevel * std::exp2(-kLongTrebleTaperOct * std::max(0.0f, octaves_from_c4));
  const float long_f1 = kLongFirstHzC4 * std::exp2(kLongFirstOct * octaves_from_c4);
  long_prev_ = 0.0f;
  long_hp_a_ =
      std::clamp(1.0f - std::exp(-kTwoPi * kLongDriveHpHz / static_cast<float>(sr)), 0.0f, 1.0f);
  long_x1_ = 0.0f;
  long_x2_ = 0.0f;
  for (int i = 0; i < kLongitudinalModes; ++i) {
    LongMode& m = long_modes_[static_cast<size_t>(i)];
    m = LongMode{};
    const float f = long_f1 * static_cast<float>(i + 1);
    if (long_level_ <= 0.0f || f >= 0.45f * static_cast<float>(sr)) continue;
    const float w = kTwoPi * f / static_cast<float>(sr);
    // The higher modes are lossier, as they are on the transverse side.
    const float t60 = std::max(0.01f, kLongT60S / static_cast<float>(i + 1));
    const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    // Peak-normalized, then rolled off along the series: the higher
    // longitudinal modes are both less excited and lossier, and left at equal
    // peaks the bank reads as a bright metallic ring rather than as the body
    // of a low note.
    m.gain = d_mag / std::max(2.0f * std::sin(w), 1.0e-6f) / static_cast<float>(i + 1);
  }
  noise_decay_ = std::exp(-1000.0f / (kStrikeNoiseTauMs * static_cast<float>(sr)));
  noise_samples_ = static_cast<int>(kStrikeNoiseMaxMs * 0.001f * sr);
  noise_pos_ = 0;
  noise_lp_ = 0.0f;
  noise_lp2_ = 0.0f;
  noise_lp3_ = 0.0f;
  noise_low_ = 0.0f;
  // The string-injected share is highpassed above the fundamental: the scrub
  // noise seeds the upper partials at random phase (the desired sheen), but a
  // random-phase component ON h1 vector-cancels against the coherent pulse —
  // an audible amplitude notch a few tens of ms into the note.
  noise_hp_a_ =
      std::clamp(1.0f - std::exp(-kTwoPi * 1.2f * f0 / static_cast<float>(sr)), 0.0f, 1.0f);
  noise_rng_ = static_cast<uint32_t>(seed ^ (seed >> 32) ^ 0x9E3779B9u) | 1u;
  const float hf_dyn = std::clamp(params.attack_hf_dynamics, 0.0f, 8.0f);
  scrub_hf_gain_ = hf_dyn > 0.0f ? std::pow(vel01 / kHammerMfVel, hf_dyn) : 1.0f;
  if (scrub_hf_gain_ != 1.0f) {
    const float w0 = rt::frequency_to_w0(kScrubHfShelfHz, static_cast<double>(sr));
    const float half_db = 10.0f * std::log10(scrub_hf_gain_);
    for (rt::BiquadState& st : scrub_hi_) {
      st.set(rt::rbj_high_shelf(w0, kButterworthQ, half_db));
      st.reset();
    }
  }
  // Below C4 the injection GROWS instead: the massive bass hammer's felt
  // scrub and re-strike chatter seed the dense h8-h20 partial cloud a wound
  // string radiates (absent it, the bass is a clean plucked stack).
  noise_inject_ =
      kStrikeNoiseInject * std::exp2(-kInjectTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                                     kInjectBassBoostOct * std::max(0.0f, -octaves_from_c4));

  // Radiation highpass coefficients (cascaded RBJ highpasses) and state. The
  // section Qs are the fourth-order Butterworth pair, so the passband stays
  // flat through the tenor while the stopband falls at the measured slope.
  {
    const float w = kTwoPi * kRadiationHpHz / static_cast<float>(sr);
    for (int i = 0; i < kRadiationHpSections; ++i) {
      HpSection& s = hp_[static_cast<size_t>(i)];
      const rt::BiquadCoeffs c = rt::rbj_highpass(w, kRadiationHpSectionQ[static_cast<size_t>(i)]);
      s.b0 = c.b0;
      s.b1 = c.b1;
      s.a1 = c.a1;
      s.a2 = c.a2;
      s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
    }
  }

  // Bridge-hill emphasis coefficients (RBJ peaking) and state.
  {
    const rt::BiquadCoeffs c = rt::rbj_peak(kTwoPi * kBridgeHillHz / static_cast<float>(sr),
                                            kBridgeHillQ, kBridgeHillGainDb);
    bh_b0_ = c.b0;
    bh_b1_ = c.b1;
    bh_b2_ = c.b2;
    bh_a1_ = c.a1;
    bh_a2_ = c.a2;
    bh_x1_ = bh_x2_ = bh_y1_ = bh_y2_ = 0.0f;
  }

  // Every string is read at one sample or more and long enough to hold its pitch, whether the
  // loop or the modal bank is the one speaking: the bank is built from the same periods.
  LoopBudget budget;
  for (int i = 0; i < num_strings_; ++i) {
    const String& s = strings_[static_cast<size_t>(i)];
    budget = worst_loop_budget(budget, loop_budget(s.base_period, s.comp, 1.0f, sr));
  }
  return budget;
}

}  // namespace sonare::midi::synth
