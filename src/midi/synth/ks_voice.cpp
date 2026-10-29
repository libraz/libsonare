#include "midi/synth/ks_voice.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "midi/synth/pitch.h"
#include "rt/dispersion.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

using sonare::constants::kDefaultDawSampleRate;

namespace {

using sonare::constants::kMidiA4;
using sonare::constants::kPi;
using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kTwoPi;

/// Tension modulation: the attack pitch rise at full velocity / full knob
/// (cents), the explicit safety clamp on that rise, and its relaxation time.
SONARE_TUNABLE(kKsTensionCentsAtFull, 55.0f);
SONARE_TUNABLE(kKsTensionMaxCents, 65.0f);
SONARE_TUNABLE(kKsTensionRelaxMs, 45.0f);

/// Steel-string inharmonicity coefficient B(note): the plain/lightly-wound
/// steel strings of an acoustic-steel or electric guitar are far less stiff
/// than a piano wire, so B is roughly a fifth of the piano's, rising toward the
/// treble. Scaled by the patch dispersion knob; nylon leaves dispersion at 0.
SONARE_TUNABLE(kBAtA4, 1.2e-4f);
SONARE_TUNABLE(kBetaPerSemitone, 0.0578f);  // ~2x per octave

float ks_steel_inharmonicity_b(uint8_t note) noexcept {
  const float n = static_cast<float>(note & 0x7Fu);
  return std::max(1.0e-5f, kBAtA4 * std::exp(kBetaPerSemitone * (n - kMidiA4)));
}

/// Second (horizontal) polarization detune, and the fret-gap reflection left
/// after the slap/pop limiter clips the string against the fret.
SONARE_TUNABLE(kPolDetuneCents, 11.0f);
SONARE_TUNABLE(kReflect, 0.06f);  // near-hard clip at the fret gap
/// What fraction of the played plane's decay targets the horizontal one keeps.
/// Applied to both its t60 and its ring at the quote frequency, so the plane
/// that dies first also loses its highs first whatever the string is doing.
SONARE_TUNABLE(kPolT60Fraction, 0.55f);
/// How much darker the horizontal plane's pole is where there is no quote
/// frequency to solve against and the tone knob still sets it.
SONARE_TUNABLE(kPolToneDarken, 0.12f);

/// KS noise draws live far above the voice-level draw indices (detune/phase/
/// drift use 0..~103 on the same per-voice seed).
constexpr uint64_t kNoiseIndexBase = 1ull << 16;
/// Key-off damper-noise draws sit well above the excitation burst so the two
/// seeded streams never overlap.
constexpr uint64_t kKeyoffNoiseIndexBase = 1ull << 20;
/// Key-off damper thump: burst length and lowpass corner (a soft felt "thunk").
SONARE_TUNABLE(kKsKeyoffMs, 18.0f);
SONARE_TUNABLE(kKsKeyoffCutoffHz, 2200.0f);

/// Pick/finger contact-click noise draws sit past the key-off stream so the
/// three seeded bursts (excitation, key-off, pick) never overlap.
constexpr uint64_t kPickNoiseIndexBase = 1ull << 24;
/// Pick-click burst length and lowpass corner. Shorter and brighter than the
/// key-off thump above: a pick or fingernail scraping the string is a harder,
/// higher-frequency contact than a felt damper falling onto it.
SONARE_TUNABLE(kKsPickNoiseMs, 25.0f);
SONARE_TUNABLE(kKsPickNoiseCutoffHz, 6000.0f);

/// Harmonics of the string read when this strike's burst is levelled, and the span of a note
/// over which the level is judged.
constexpr int kLevelHarmonics = 128;
constexpr float kLevelHorizonS = 0.25f;

/// Where ks.hf_decay_s is quoted. A fixed frequency for the reason string_loop.h
/// gives: a target quoted at the octave is unreachable in the bass.
SONARE_TUNABLE(kKsHfQuoteHz, 4000.0f);

/// Where the optional mid-band anchor (ks.mid_decay_s) is quoted, between the
/// fundamental and kKsHfQuoteHz — where a wound string's internal
/// (viscoelastic) loss starts to dominate the support loss below it.
SONARE_TUNABLE(kKsMidQuoteHz, 1000.0f);

/// Where the pick and excitation key tracks are quoted (A2): low in every compass this core
/// plays, so a pick law capped at the midpoint can put its knee at any note above it.
constexpr float kKsPluckQuoteHz = 110.0f;

/// How many times faster the partial at ks.mute_harmonic decays than the
/// fundamental, when a hand is on the strings. The captured palm mute puts its
/// break at the fourth partial and drops it from 18 dB/s to 68.
SONARE_TUNABLE(kKsMuteDecayRatio, 3.8f);

}  // namespace

void KsVoiceCore::start(const KsPatchParams& params, double sample_rate, uint8_t note,
                        Velocity16 velocity, uint64_t seed) noexcept {
  const double host_sr = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  // A loop too short to hold its pitch and decay at the host rate runs at a multiple of it;
  // everything below is built for that internal rate.
  const int factor = settle_loop_oversample(
      [&](int f) { return configure(params, host_sr * f, note, velocity, seed); });
  decimator_.configure(factor);
}

LoopBudget KsVoiceCore::configure(const KsPatchParams& params, double sr, uint8_t note,
                                  Velocity16 velocity, uint64_t seed) noexcept {
  noise_ = VoiceRandomSequence(seed);
  killed_ = false;

  const float f0 = note_to_hz(note);
  const float base_period = static_cast<float>(sr) / f0;

  // A finger at 1/N forces a node, so the string sounds in N segments and every
  // loop below shortens with it. The pluck still grips the full string, which is
  // why the excitation keeps base_period.
  const float node = std::floor(std::clamp(params.harmonic_node, 0.0f, 8.0f));
  const float loop_period = node >= 2.0f ? base_period / node : base_period;

  // Decay: t60 stretched per octave below A4 (low strings ring longer).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (kMidiA4 - static_cast<float>(note & 0x7Fu)) / kSemitonesPerOctave;
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);
  const float damped_t60 = std::max(0.01f, params.release_damp_s);

  // Loop lowpass: brightness -> feedback coefficient a (y += (1-a)(x-y)), voiced
  // at kLossVoicedSr.
  const float tone_a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f;
  const float fixed_quote_w = kTwoPi * kKsHfQuoteHz / static_cast<float>(sr);
  const float mute = std::clamp(params.mute_harmonic, 0.0f, 16.0f);
  // Sets one loop up from its t60, quote-frequency ring and optional mid anchor.
  auto voice_loop = [&](StringLoop& loop, float* span, float period, float t60_s, float hf_t60_s,
                        float mid_t60_s = 0.0f, float tone_a_offset = 0.0f) noexcept {
    float a = loss_pole_at_rate(std::min(0.97f, tone_a + tone_a_offset), sr);
    // The read's magnitude at the fundamental is paid from the target before any filter is
    // solved; every other target of this loop grows with it so their ratios stay as asked.
    const float repay = interpolation_repayment(string_loop_gain_for(period, sr, t60_s), period, a);
    float g = string_loop_gain_for(period, sr, t60_s) * repay;
    float release_g = std::min(0.9999f, string_loop_gain_for(period, sr, damped_t60) * repay);
    const float w0 = kTwoPi / period;
    // A hand mute is measured against the note and the open string against the
    // room: a palm damps a mode by how far it moves under it, so both where the
    // break falls and how deep it is scale with the string, while a wire's own
    // losses and the air's are a property of the frequency.
    const float quote_w = mute > 0.0f ? std::min(mute * w0, 0.9f * kPi) : fixed_quote_w;
    const float quote_t60 = mute > 0.0f ? t60_s / kKsMuteDecayRatio : hf_t60_s;
    if ((hf_t60_s > 0.0f || mute > 0.0f) && quote_w > w0 * 1.5f) {
      const float g_ref = string_loop_gain_for(period, sr, quote_t60) * repay;
      const float mid_w = kTwoPi * kKsMidQuoteHz / static_cast<float>(sr);
      const float g_mid = mute <= 0.0f && mid_t60_s > 0.0f
                              ? string_loop_gain_for(period, sr, mid_t60_s) * repay
                              : 0.0f;
      const QuotedStringLoopFilter solved =
          solve_quoted_loop_filter(w0, quote_w, g, g_ref, mid_w, g_mid);
      release_g = std::min(0.9999f, release_g * solved.release_scale);
      loop.configure_filter(span, capacity_, period, solved.a, solved.g, release_g,
                            solved.has_pole2, solved.a2, solved.g2);
      return solved.a;
    }
    loop.configure_filter(span, capacity_, period, a, g, release_g);
    return a;
  };

  // The played string: the vertical plane the pluck grips. configure_filter()
  // tunes it by compensating the EXACT phase delay of the loop filter at the
  // fundamental (not just its DC group delay) plus the one-sample feedback path,
  // so the sounding pitch matches the note to a few cents.
  const float hf_t60 = std::max(0.0f, params.hf_decay_s);
  const float mid_t60 = std::max(0.0f, params.mid_decay_s);
  const float a = voice_loop(string_, slab_, loop_period, t60, hf_t60, mid_t60);

  // Stiff-string dispersion (steel strings). 0 disables the allpass cascade so
  // the loop stays a harmonic string, bit-identical. Otherwise scale the steel
  // inharmonicity into an in-loop allpass (the shared piano dispersion solver)
  // and fold its phase delay into the loop compensation so f0 tuning holds.
  const float omega = kTwoPi / loop_period;
  const float tau_lp = onepole_group_delay_samples(a, omega);
  disp_a_ = 0.0f;
  for (float& s : disp_state_) s = 0.0f;
  const float dispersion = std::clamp(params.dispersion, 0.0f, 1.0f);
  if (dispersion > 0.0f) {
    const float b_coeff = dispersion * ks_steel_inharmonicity_b(note);
    const float phase_budget = loop_period - 4.0f - tau_lp;
    disp_a_ = rt::dispersion_allpass_a(b_coeff, omega, a, kKsDispersionStages, phase_budget);
    if (disp_a_ != 0.0f) {
      string_.loop_comp +=
          static_cast<float>(kKsDispersionStages) * rt::allpass_phase_delay(disp_a_, omega);
    }
  }

  // Fret-slap: a narrower displacement gap for higher intensity. 0 disables the
  // limiter entirely so the render path stays bit-identical to the plain string.
  const float slap = std::clamp(params.slap, 0.0f, 1.0f);
  slap_threshold_ = slap > 0.0f ? 0.55f - 0.35f * slap : 0.0f;

  // Excitation: one period of seeded noise through the pick-position comb and
  // the velocity-driven dynamic-level lowpass (hard pluck = bright).
  exc_total_ = std::max(8, static_cast<int>(base_period));
  exc_pos_ = 0;
  // Octaves from the quote note; both pluck key tracks are 2^(slope * this), exactly 1 at slope 0.
  const float pluck_octaves = std::log2(f0 / kKsPluckQuoteHz);
  const float pick_track =
      std::exp2(std::clamp(params.pick_position_per_octave, 0.0f, 4.0f) * pluck_octaves);
  pick_delay_ = static_cast<int>(
      std::clamp(params.pick_position * pick_track, 0.0f, 0.5f) * base_period + 0.5f);
  const float vel01 = velocity.f7() / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float bright =
      std::clamp(params.exc_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);
  // Exponential brightness -> cutoff map (300 Hz .. ~12 kHz) through two
  // cascaded one-poles, so the velocity swing is clearly audible.
  const float exc_cutoff =
      300.0f * std::exp2(5.3f * bright) *
      std::exp2(std::clamp(params.exc_brightness_per_octave, 0.0f, 4.0f) * pluck_octaves);
  exc_alpha_ = lowpass_alpha_at_rate(exc_cutoff, sr);
  exc_noise_gain_ =
      noise_gain_at_rate(sr) *
      burst_level_correction(sr, loop_period, string_loop_gain_for(loop_period, sr, t60), a);
  exc_lp1_ = 0.0f;
  exc_lp2_ = 0.0f;

  // Physical-pluck doublet width. 0 disables it so the burst stays the
  // plain seeded noise (bit-identical). The finger's release-velocity lobe is
  // wide and round; a nail or pick releases through a much narrower, brighter
  // edge, so nail shrinks the lobe from ~0.9 down to ~0.15 of the burst period.
  pluck_style_ = std::clamp(params.pluck_style, 0.0f, 1.0f);
  if (pluck_style_ > 0.0f) {
    const float nail = std::clamp(params.nail, 0.0f, 1.0f);
    const float frac = 0.9f - 0.75f * nail;
    pluck_contact_ = std::max(4, static_cast<int>(frac * static_cast<float>(exc_total_)));
  } else {
    pluck_contact_ = 0;
  }

  // A body knock's reference: (2/N)|E(2pi/N)| of exactly what render_internal() injects.
  pluck_fundamental_ = 0.0f;
  if (params.body_knock > 0.0f) {
    const double coeff = 2.0 * std::cos(constants::kTwoPiD / static_cast<double>(base_period));
    double s1 = 0.0, s2 = 0.0;
    float lp1 = 0.0f, lp2 = 0.0f;
    for (int pos = 0; pos < exc_total_ + pick_delay_; ++pos) {
      const double next = static_cast<double>(excitation_step(pos, lp1, lp2)) + coeff * s1 - s2;
      s2 = s1;
      s1 = next;
    }
    const double power = std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2);
    pluck_fundamental_ =
        static_cast<float>(2.0 * std::sqrt(power) / static_cast<double>(base_period));
  }

  // Magnetic pickup (electric). 0 disables it so the output stays bit-identical.
  // The pickup senses the string at a fixed point: a second, output-side comb
  // with a node at pickup_pos of the period, plus a mild field-gradient
  // nonlinearity (even harmonics). The tap is at least a few samples so the
  // read-only loop tap never lands on the pending write.
  const float pickup = std::clamp(params.pickup_pos, 0.0f, 0.5f);
  if (pickup > 0.0f) {
    // The tap sits pickup * period ahead of the loop's own read, which is the period
    // less the delay the loop filter and dispersion carry: a pickup near the bridge
    // (small pickup) combs at a short delay (its first peak high = bright), a neck
    // pickup at a longer delay (rounder).
    const float offset = std::max(4.0f, string_.effective_delay(1.0f) - pickup * string_.period);
    pickup_delay_q8_ = static_cast<int>(offset * 256.0f);
    pickup_fraction_ = pickup;
    pickup_depth_ = 0.85f;  // near-full comb notch depth
    pickup_mag_ = 0.18f;    // gentle even-harmonic nonlinearity
  } else {
    pickup_fraction_ = 0.0f;
    pickup_delay_q8_ = 0;
    pickup_depth_ = 0.0f;
    pickup_mag_ = 0.0f;
  }

  // Tension modulation. 0 disables it so the pitch path stays bit-identical. A
  // hard pluck starts a touch sharp and relaxes back: the rise is velocity-
  // scaled and clamped explicitly in cents (not left to the delay-line clamp),
  // then relaxed toward the nominal pitch by an exponential envelope.
  const float tension = std::clamp(params.tension_mod, 0.0f, 1.0f);
  if (tension > 0.0f) {
    const float rise_cents = std::min(kKsTensionMaxCents, tension * vel01 * kKsTensionCentsAtFull);
    tension_ratio_peak_ = cents_to_ratio(rise_cents) - 1.0f;
    tension_env_ = 1.0f;
    tension_decay_coeff_ =
        std::exp(-1.0f / std::max(1.0f, kKsTensionRelaxMs * 0.001f * static_cast<float>(sr)));
  } else {
    tension_ratio_peak_ = 0.0f;
    tension_env_ = 0.0f;
    tension_decay_coeff_ = 0.0f;
  }

  // Second (horizontal) polarization (off unless params.polarization > 0 ->
  // render skips it, primary path bit-identical). A second loop detuned a few
  // cents sharp, more damped and decaying faster than the primary: the two
  // planes beat and the faster line dies first (two-stage decay).
  const float polarization = std::clamp(params.polarization, 0.0f, 1.0f);
  if (polarization > 0.0f && slab_ != nullptr) {
    const float pol_period = loop_period / cents_to_ratio(kPolDetuneCents);
    // The horizontal plane takes the same fraction off both its targets: it
    // decays faster than the primary and loses its highs faster still, which is
    // what gives the two-stage decay.
    voice_loop(pol_, slab_ + capacity_, pol_period, kPolT60Fraction * t60, kPolT60Fraction * hf_t60,
               kPolT60Fraction * mid_t60, kPolToneDarken);
    // The damper grips both planes at once, so the horizontal one is released at
    // the played string's damped gain rather than at one solved for its own
    // (slightly shorter) period.
    pol_.release_gain = string_.release_gain;
    pol_exc_ = 0.6f;  // the pluck grips the vertical plane; the horizontal weakly
    pol_couple_ = polarization;

    // Bridge coupling (off unless body_coupling > 0): a share of the largest
    // exchange the two planes can take and stay inside the unit circle.
    const float bc = std::clamp(params.body_coupling, 0.0f, 1.0f);
    if (bc > 0.0f) {
      couple_gain_ = bc * bridge_coupling_limit(string_, pol_);
    } else {
      couple_gain_ = 0.0f;
    }
  } else {
    pol_.disable();
    pol_couple_ = 0.0f;
    couple_gain_ = 0.0f;
  }

  // Octave-up 4' companion line (the harpsichord 4' register). Off unless
  // params.octave_mix > 0 -> render skips it, primary path bit-identical. A
  // third loop at exactly half the primary period (an octave up), plucked by
  // the same key but its own jack: it shares the excitation and sums into the
  // output, reinforcing the octave like a real coupled 4' choir.
  const float octave_mix = std::clamp(params.octave_mix, 0.0f, 1.0f);
  if (octave_mix > 0.0f && slab_ != nullptr) {
    // The same decay targets as the primary, solved at the octave-up period so
    // both the 4' pitch and its loss are right for the shorter string.
    voice_loop(oct_, slab_ + 2 * capacity_, 0.5f * loop_period, t60, hf_t60, mid_t60);
    oct_exc_ = 0.7f;  // the 4' jack grips its string a touch less than the 8'
    oct_couple_ = octave_mix;
  } else {
    oct_.disable();
    oct_couple_ = 0.0f;
  }

  // Key-off / damper noise. 0 disables it (release() never arms the burst, output
  // bit-identical). Precompute the burst length and its lowpass corner; the burst
  // itself is triggered at note-off. A single one-pole lowpass straight to the
  // mix -> the exact per-pole correction, not the PSD approximation.
  keyoff_.configure(std::clamp(params.keyoff_noise, 0.0f, 1.0f), kKsKeyoffMs, kKsKeyoffCutoffHz, sr,
                    false);  // inactive until release()

  // Pick/finger contact click, armed at note-on; 0 disables it (bit-identical).
  pick_.configure(std::clamp(params.pick_noise, 0.0f, 1.0f) * vel01, kKsPickNoiseMs,
                  kKsPickNoiseCutoffHz, sr, true);

  // Every loop in use is read at one sample or more and long enough to hold its pitch.
  LoopBudget budget = loop_budget(loop_period, string_.loop_comp, 1.0f, sr);
  if (pol_couple_ > 0.0f) {
    budget = worst_loop_budget(budget, loop_budget(loop_period / cents_to_ratio(kPolDetuneCents),
                                                   pol_.loop_comp, 1.0f, sr));
  }
  if (oct_couple_ > 0.0f) {
    budget = worst_loop_budget(budget, loop_budget(0.5f * loop_period, oct_.loop_comp, 1.0f, sr));
  }
  return budget;
}

// Bounded and allocation-free: at most kLevelHarmonics harmonics over one burst, measured about
// 50 us at E1 and 48 kHz.
float KsVoiceCore::burst_level_correction(double sr, float loop_period, float loop_gain,
                                          float loop_pole) const noexcept {
  const double a = static_cast<double>(exc_alpha_);
  const double p = 1.0 - a;
  const double pole = static_cast<double>(loop_pole);
  const int limit =
      std::min(kLevelHarmonics,
               static_cast<int>(std::floor(0.5 * (static_cast<double>(loop_period) - 1.0))));
  if (limit < 1) return 1.0f;

  // Per harmonic of the loop: how long it keeps ringing inside the horizon (the integral of its
  // decay, in passes) times the energy the comb and the two excitation poles let through. The
  // draw is white, so a harmonic's expected energy is that response times exc_total_ / 3.
  std::array<double, kLevelHarmonics> weight{}, response{};
  const double horizon_passes =
      static_cast<double>(kLevelHorizonS) * sr / static_cast<double>(loop_period);
  double total = 0.0;
  for (int k = 1; k <= limit; ++k) {
    const double w = constants::kTwoPiD * k / static_cast<double>(loop_period);
    const double loop_mag = static_cast<double>(loop_gain) * (1.0 - pole) /
                            std::hypot(1.0 - pole * std::cos(w), pole * std::sin(w));
    const double lambda = -std::log(std::max(loop_mag, 1.0e-9));
    const double ring =
        -std::expm1(-2.0 * lambda * horizon_passes) / (2.0 * std::max(lambda, 1.0e-9));
    const double lp = a / std::hypot(1.0 - p * std::cos(w), p * std::sin(w));
    const double comb =
        pick_delay_ > 0 ? 2.0 - 2.0 * std::cos(w * static_cast<double>(pick_delay_)) : 1.0;
    weight[static_cast<size_t>(k - 1)] = ring;
    response[static_cast<size_t>(k - 1)] = lp * lp * lp * lp * comb;
    total += ring * response[static_cast<size_t>(k - 1)];
  }
  // Harmonics past the point where the rest holds under half a percent are not read.
  int harmonics = limit;
  for (double tail = 0.0; harmonics > 1; --harmonics) {
    tail +=
        weight[static_cast<size_t>(harmonics - 1)] * response[static_cast<size_t>(harmonics - 1)];
    if (tail > 0.005 * total) break;
  }

  // This strike: the raw draw through the comb and the two poles, as render_internal() runs it,
  // read at each harmonic with a Goertzel recurrence (only the magnitude is wanted).
  std::array<double, kLevelHarmonics> coeff{}, s1{}, s2{};
  double expected = 0.0;
  for (int k = 0; k < harmonics; ++k) {
    const auto i = static_cast<size_t>(k);
    coeff[i] = 2.0 * std::cos(constants::kTwoPiD * (k + 1) / static_cast<double>(loop_period));
    expected += weight[i] * response[i];
  }
  expected *= exc_total_ / 3.0;
  const int window = exc_total_ + pick_delay_;
  double l1 = 0.0, l2 = 0.0;
  for (int pos = 0; pos < window; ++pos) {
    double burst =
        pos < exc_total_ ? noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(pos)) : 0.0;
    if (pick_delay_ > 0 && pos >= pick_delay_) {
      burst -= noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(pos - pick_delay_));
    }
    l1 += a * (burst - l1);
    l2 += a * (l1 - l2);
    for (int k = 0; k < harmonics; ++k) {
      const auto i = static_cast<size_t>(k);
      const double next = l2 + coeff[i] * s1[i] - s2[i];
      s2[i] = s1[i];
      s1[i] = next;
    }
  }
  double actual = 0.0;
  for (int k = 0; k < harmonics; ++k) {
    const auto i = static_cast<size_t>(k);
    actual += weight[i] * (s1[i] * s1[i] + s2[i] * s2[i] - coeff[i] * s1[i] * s2[i]);
  }
  return actual > 1.0e-12 ? static_cast<float>(std::sqrt(expected / actual)) : 1.0f;
}

// The seeded noise burst, optionally crossfaded toward a deterministic pluck doublet;
// pluck_style_ == 0 returns the raw noise so the path is bit-identical.
float KsVoiceCore::excitation_source(int k) const noexcept {
  const float nz = exc_noise_gain_ * noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(k));
  if (pluck_style_ <= 0.0f) return nz;
  // Raised-cosine up/down lobe: the finger's release-velocity pulse. Zero-
  // mean (the second half is negated), width pluck_contact_ (narrow = bright).
  float pluck = 0.0f;
  if (k < pluck_contact_) {
    const float win = 0.5f * (1.0f - std::cos(kTwoPi * (static_cast<float>(k) + 1.0f) /
                                              static_cast<float>(pluck_contact_ + 1)));
    pluck = (k < pluck_contact_ / 2) ? win : -win;
  }
  return nz + pluck_style_ * (pluck - nz);
}

// Pick-position comb burst[n] - burst[n - pick_delay], whose delayed copy runs pick_delay
// samples past the burst so the notches stay exact, then the two excitation poles.
float KsVoiceCore::excitation_step(int pos, float& lp1, float& lp2) const noexcept {
  float burst = pos < exc_total_ ? excitation_source(pos) : 0.0f;
  if (pick_delay_ > 0 && pos >= pick_delay_) {
    burst -= excitation_source(pos - pick_delay_);
  }
  lp1 += exc_alpha_ * (burst - lp1);
  lp2 += exc_alpha_ * (lp1 - lp2);
  return 0.7f * lp2;  // comb headroom
}

float KsVoiceCore::render(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  return decimator_.run([&] { return render_internal(pitch_ratio); });
}

float KsVoiceCore::render_internal(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  if (string_.buffer == nullptr || string_.size < 8) return 0.0f;

  float exc = 0.0f;
  if (exc_pos_ < exc_total_ + pick_delay_) {
    exc = excitation_step(exc_pos_, exc_lp1_, exc_lp2_);
    ++exc_pos_;
  }

  // pitch_ratio scales the frequency, so it divides the loop delay.
  float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  // Tension modulation: a hard pluck starts sharp and relaxes back. The rise is
  // cents-clamped at start(), so this only decays it; skipped (bit-identical)
  // when tension is off. Applied to the shared ratio so both planes track it.
  if (tension_ratio_peak_ != 0.0f && tension_env_ > 1.0e-4f) {
    ratio *= 1.0f + tension_ratio_peak_ * tension_env_;
    tension_env_ *= tension_decay_coeff_;
  }
  float fb = string_.feedback();
  // Bridge coupling: the horizontal plane feeds a little energy back into the
  // vertical one (0 unless body_coupling engaged the 2x2 admittance). Gated so
  // the plain path stays bit-identical when the bridge is off.
  if (couple_gain_ != 0.0f) fb += couple_gain_ * pol_.lp_state;
  float loop_in = exc + fb;
  if (slap_threshold_ > 0.0f) {
    // Fret contact: the string cannot swing past the fret gap. Over-travel is
    // hard-limited with only a sliver of give, so the clipped tops generate the
    // odd-harmonic buzz of the slap/pop attack (a memoryless nonlinear limit).
    const float th = slap_threshold_;
    if (loop_in > th) {
      loop_in = th + (loop_in - th) * kReflect;
    } else if (loop_in < -th) {
      loop_in = -th + (loop_in + th) * kReflect;
    }
  }
  // Magnetic-pickup position tap: a read-only second tap of the loop line at
  // the pickup point, taken before the write so it never reads the pending
  // sample (0 unless a pickup is engaged).
  float pickup_tap = 0.0f;
  if (pickup_depth_ != 0.0f) {
    int pickup_delay_q8 = pickup_delay_q8_;
    if (ratio != 1.0f) {
      // Bent or tensioned: re-place the tap against the main read so the notch follows the pitch.
      const float nominal =
          string_.effective_delay(ratio) - pickup_fraction_ * string_.period / ratio;
      const float max_delay = static_cast<float>(std::max(4, string_.size - 4));
      const float delay = std::clamp(nominal, 4.0f, max_delay);
      pickup_delay_q8 = static_cast<int>(delay * 256.0f);
    }
    pickup_tap = rt::lagrange3_read(string_.buffer, static_cast<size_t>(string_.size),
                                    string_.write, pickup_delay_q8);
  }

  const float out = string_.advance(loop_in, ratio);
  // Stiff-string dispersion: an allpass cascade in the loop makes the highs
  // travel faster, stretching the partials sharp. Skipped when disp_a_ == 0 so
  // the loop lowpass sees the plain delayed sample (bit-identical).
  float shaped = out;
  if (disp_a_ != 0.0f) {
    for (float& state : disp_state_) {
      const float y = disp_a_ * shaped + state;
      state = shaped - disp_a_ * y;
      shaped = y;
    }
  }
  string_.commit(shaped);

  float result;
  if (pol_couple_ > 0.0f) {
    // Horizontal polarization: a detuned loop sharing the pluck; its output
    // sums into the mix, beating against the primary (two-stage decay).
    float pol_in = pol_exc_ * exc + pol_.feedback();
    // Reciprocal bridge return: the vertical plane feeds the horizontal one.
    if (couple_gain_ != 0.0f) pol_in += couple_gain_ * string_.lp_state;
    result = out + pol_couple_ * pol_.process(pol_in, ratio);
  } else {
    result = out;
  }

  if (oct_couple_ > 0.0f) {
    // Octave-up 4' companion: a half-period loop sharing the pluck; its output
    // sums into the mix, reinforcing the octave (the coupled 4' register).
    const float oct_in = oct_exc_ * exc + oct_.feedback();
    result += oct_couple_ * oct_.process(oct_in, ratio);
  }

  if (keyoff_.active()) {
    // Key-off damper thump, armed at note-off (never armed when its amount is 0).
    keyoff_.add_to(result,
                   noise_.bipolar_at(kKeyoffNoiseIndexBase + static_cast<uint64_t>(keyoff_.pos)));
  }

  if (pick_.active()) {
    // Pick contact click, added to the output independently of the loop.
    pick_.add_to(result, noise_.bipolar_at(kPickNoiseIndexBase + static_cast<uint64_t>(pick_.pos)));
  }

  if (pickup_depth_ != 0.0f) {
    // Magnetic pickup: the output-side position comb notches the harmonics with
    // a node at the pickup point, then the field-gradient nonlinearity adds the
    // even harmonics of the string-to-voltage transfer (the electric-guitar
    // character; amp/overdrive is downstream, not here).
    float y = result - pickup_depth_ * pickup_tap;
    y += pickup_mag_ * y * y;
    result = y;
  }
  return result;
}

void KsVoiceCore::release() noexcept {
  string_.release();
  if (pol_couple_ > 0.0f) pol_.release();
  if (oct_couple_ > 0.0f) oct_.release();
  // Arm the key-off damper thump (no-op when the burst is disabled).
  if (keyoff_.amount > 0.0f) keyoff_.arm();
}

void KsVoiceCore::kill() noexcept {
  killed_ = true;
  decimator_.reset();
  exc_pos_ = exc_total_;
  string_.kill();
  pol_.kill();
  oct_.kill();
  keyoff_.pos = keyoff_.len;
  pick_.pos = pick_.len;
}

}  // namespace sonare::midi::synth
