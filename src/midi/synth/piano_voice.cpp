#include "midi/synth/piano_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/voice_random.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kPi;
using sonare::constants::kTwoPi;

/// Mezzo-forte reference velocity (0..1) the felt-hammer laws are anchored at.
constexpr float kHammerMfVel = 0.6f;
/// Additional felt-stiffness cutoff octaves per unit velocity above mf, per
/// unit hammer_dynamics: a compressed felt patch passes more of the pulse top.
constexpr float kHammerDynBrightOct = 1.5f;
/// Hammer-contact floor in fundamental PERIODS, anchored at C4 and graded
/// per octave (signed: it shrinks into the bass, grows into the treble). A
/// real grand's contact spans ~0.5 of the period at C4 and more than a full
/// period in the treble; the contact duration is THE felt-vs-nail cue — a
/// contact much shorter than half a period injects a spike-like pulse that
/// reads as a fingernail pluck, and in the treble it additionally leaves the
/// overtones louder than the fundamental (a plucked-string spectrum).
constexpr float kContactPeriodsAtC4 = 0.503038f;
constexpr float kContactPeriodsPerOct = 0.613525f;
constexpr float kContactPeriodsMax = 2.0f;
/// Treble decay taper (halvings of the aftersound stage per octave above C4):
/// the short, stiff, heavily-damped treble strings die far faster than the
/// tenor. Applied to the slow stage only — the prompt-sound rate is set by the
/// polarization/unison coupling, which has its own register profile below.
constexpr float kTrebleDecayOct = 1.94164f;
/// Register profile of the prompt-vs-aftersound contrast. The double decay is
/// strongest in the trichord mid-range (vertical polarization + unison
/// coupling drain the bridge fast, then the decohered residue rings): the
/// wound bass strings have no unison partner and ring at essentially the
/// aftersound rate, and the capped treble is so short-lived the two stages
/// merge. Gaussian in octaves from C4; at the edges the effective prompt rate
/// relaxes toward the aftersound rate.
constexpr float kTwoStageWidthOct = 0.616718f;
/// Treble taper cap (octaves above C4): the decay/darkening keytracks stop
/// steepening past here — an uncapped taper leaves the top octave with a
/// sub-100 ms husk of a note.
constexpr float kTrebleTaperOctCap = 1.5f;
/// Treble loop darkening (effective-brightness drop per octave above C4): the
/// treble string's upper partials decay much faster than its fundamental, so
/// the loop lowpass closes toward the top even for a bright patch voicing.
/// Kept gentle: over-closing leaves a 3-4 partial flageolet instead of a
/// piano treble.
constexpr float kTrebleBrightPerOct = 0.06f;
/// Effective-brightness drop per octave BELOW C4 (wound-string mid-partial
/// damping; see bright_eff). The h1 decay is unaffected — the loop-lowpass
/// loss at the fundamental is compensated (lp_comp), so this only shortens
/// the upper partials.
constexpr float kBassDarkPerOct = 0.15f;
/// String-to-string inharmonicity spread inside a unison (fractional jitter
/// on the dispersion allpass): real unison strings never share an exact B, so
/// each partial's unison beat runs at its own rate. Identical coefficients
/// make every partial null in lockstep — an audibly artificial hollow dip.
constexpr float kUnisonStiffJitter = 0.05f;
/// Under the soft pedal the action rides a softer, less-grooved felt patch that
/// compresses far less under a hard blow, so the velocity dynamics is scaled
/// down there (this also preserves the una-corda high-frequency softening).
constexpr float kUnaCordaDynScale = 0.4f;
/// Uneven unison strike: hammer crowning and string leveling never deliver
/// equal energy to a bichord/trichord's strings. Equal amplitudes make the
/// unison beats cancel to full-depth nulls (an audible slow chorus wobble);
/// the uneven strike keeps them as shallow ripple and seeds the aftersound.
constexpr float kUnisonStrikeUneven = 0.15f;
/// Uneven bridge coupling across the unison (Weinreich): each string meets
/// the bridge at a slightly different impedance, so the antisymmetric normal
/// mode — whose string motions cancel at an ideal bridge — still radiates,
/// at roughly this fraction of the symmetric mode. This is what makes the
/// aftersound AUDIBLE without detuning the unison out of the locked regime:
/// with equal radiation the slow mode is silent, and compensating with deep
/// detune buys the second stage at the cost of a chorus-like beating
/// fundamental no tuned piano has.
constexpr float kUnisonRadSpread = 0.6f;
/// Felt impact noise: level relative to the hammer amplitude, exponential
/// decay time, and hard stop of the burst. The noise passes the same
/// velocity-driven felt-stiffness lowpass as the pulse, so soft strikes thud
/// and hard strikes click.
constexpr float kStrikeNoiseGain = 0.6f;
constexpr float kStrikeNoiseTauMs = 8.0f;
constexpr float kStrikeNoiseMaxMs = 30.0f;
/// The impact noise radiates through a darker path than the string pulse: a
/// felt hammer lands as a 0.5-2 kHz thud, not a pick click — the noise gets
/// its own lowpass at this fraction of the felt-stiffness cutoff.
constexpr float kStrikeNoiseCutoffScale = 0.487539f;
/// Halvings of the noise cutoff per octave below C4 (see noise_cutoff).
constexpr float kNoiseCutoffBassOct = 0.5f;
/// Third noise pole, placed this factor above the main cutoff: the felt
/// noise keeps its passband but falls off a cliff past it — the reference
/// attack holds energy to a few kHz then drops ~37 dB into the next octave,
/// a shape two poles cannot make (their tail is what read as a jack click).
constexpr float kNoiseSteepRatio = 4.0f;
/// Finite hammer-head width: the felt contacts several percent of the string
/// length, and that footprint lowpasses the injected force (partials whose
/// half-wavelength fits inside the footprint cancel). Caps the pulse content
/// at about this multiple of the fundamental — without it the bass/tenor
/// overtones around h6-h12 come out plectrum-hard (a honky, dry midrange no
/// felt hammer produces). The strike-noise path is NOT capped: the scrub
/// noise is what carries the airy top sheen.
constexpr float kHammerWidthHarmonics = 2.69125f;
/// Share of the impact noise injected into the strings themselves: the felt
/// scrub and the wave re-striking the hammer during contact excite the
/// string broadband. This is what seeds the HIGH partials (h10+) — the
/// smooth force pulse alone rolls off ~18 dB/oct past ~2/contact, leaving
/// the mid/bass sustain with no top: a bright strike into a dull ring reads
/// as a fingernail pluck. Random phase, so it does not re-cohere the loop.
constexpr float kStrikeNoiseInject = 0.298027f;
/// The injection tapers above C4 (halvings per octave): the treble hammer
/// rests on the string for around a full period, shorting high-frequency
/// string motion at the contact point — broadband seeding there rings the
/// overtones into a harpsichord jangle instead of a piano treble.
constexpr float kInjectTrebleTaperOct = 0.654102f;
/// ...and grows below C4 (doublings per octave): the massive bass hammer's
/// felt scrub and re-strike chatter seed the dense partial cloud a wound
/// string radiates (absent it, the bass is a clean plucked stack).
constexpr float kInjectBassBoostOct = 1.23607f;
/// The impact-noise LEVEL also tapers above C4: the treble hammer is a few
/// grams of hard felt on a short string — its scrub is faint next to the
/// tone, where the same level against a fast-dying treble note reads as a
/// pick scratch riding every onset.
constexpr float kNoiseTrebleTaperOct = 1.08754f;
/// Hammer-knock radiation (through the soundboard) relative to the string
/// injection, and its growth per octave BELOW C4: the wide wound bass
/// strings take a massive hammer whose impact drives the board directly —
/// on a reference grand the low-register attack peaks in the 60-250 Hz
/// thump, not in the string partials. Without that boom the exposed bass
/// harmonic stack reads as a harpsichord register.
constexpr float kKnockGain = 2.6f;
/// The knock radiates only the impact THUD: the hammer/action/board contact
/// pumps a fixed low band regardless of the note (a treble strike lands as a
/// quiet thock, not a burst at the string's own pitch). Radiating the raw
/// pulse instead puts note-frequency energy straight into the board and
/// sympathetic modes, whose stretch-detuned ring then beats against the
/// string fundamental — an audible onset notch in the treble.
constexpr float kKnockThudHz = 350.0f;
/// Halvings of the thud frequency per octave below C4 (see thud_hz).
constexpr float kKnockThudBassOct = 0.7f;
/// Radiation bloom: the string radiates only through the board, whose modes
/// take time to ring up — the tone swells over tens of ms in the bass and a
/// few ms in the treble, while a plucked string (or a harpsichord jack) is
/// loudest at the very first cycle. One-pole rise time constant at C4 (ms)
/// and its per-octave keytrack (bass slower, treble faster). The knock/thud
/// path is NOT bloomed — the impact is the first thing heard.
constexpr float kBloomTauMsC4 = 4.6604f;
constexpr float kBloomTauOct = 0.9f;
/// String yield under the blow (fraction of the hammer's speed the strike
/// point recedes at, at mf peak force). Keytracked DOWN toward the treble:
/// the treble hammer outweighs its short string many times over, so the
/// string barely loads the bounce (a clean full-period dwell); the wound
/// bass strings are massive and swing away under the light-relative hammer,
/// stretching the contact and softening the transfer.
constexpr float kStringYield = 0.8f;
/// Register level compensation on the injected force (dB per octave from C4,
/// clamped at +/-1.25 oct): the bass chatter re-feeds its strings while the
/// treble's near-period dwell couples weakly into the fundamental, tilting
/// the raw physical levels bass-heavy by ~10 dB/oct against the reference.
constexpr float kInjTiltDbOct = 3.5f;
constexpr float kYieldTrebleOct = 2.0f;
constexpr float kKnockBassBoostOct = 1.3f;
/// ...and shrinks above C4: the treble hammer is a few grams — its thud is
/// far below the tone (the reference treble attack has almost no 60-250 Hz).
constexpr float kKnockTrebleTaperOct = 2.0f;
/// The hammer-width harmonic cap keytracks from C4, signed doublings per
/// octave on each side: in HARMONIC number the felt footprint's cap follows
/// both the footprint's span of the string and how the contact dwell scales
/// against the period, so neither side is forced brighter or darker a
/// priori — the reference ladders decide the sign per register.
constexpr float kWidthBassOct = -1.96668f;
constexpr float kWidthTrebleOct = 0.81966f;
/// Strike-point keytrack (doublings per octave below C4) applied to the
/// patch's strike_position fraction.
constexpr float kStrikePosBassOct = 0.556f;
/// Soundboard radiation highpass (2nd order). The board radiates poorly
/// below its first body modes, so a piano's low fundamentals barely reach
/// the air — the pitch is carried as virtual pitch by the upper partials.
/// Passing the raw string fundamental instead makes the note read as a
/// literally vibrating string (a guitar with its Helmholtz-supported lows),
/// an octave darker than a piano radiates.
constexpr float kRadiationHpHz = 95.0f;
constexpr float kRadiationHpQ = 0.6f;
/// Bridge-hill radiation emphasis (RBJ peaking biquad). The bridge/board
/// mobility of a grand peaks broadly around 1-2 kHz (the "bridge hill"),
/// lifting whichever partials land in that fixed band: the bass h9-h12
/// partial crown, the mid-register presence, and the treble's h2-h3 body all
/// radiate from this resonance, not from the strings. Without it every
/// register reads mid-heavy and boxed-in regardless of the hammer spectrum.
constexpr float kBridgeHillHz = 1485.15f;
constexpr float kBridgeHillGainDb = 9.91486f;
constexpr float kBridgeHillQ = 2.40983f;

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

}  // namespace

float piano_inharmonicity_b(uint8_t note) noexcept {
  const float n = static_cast<float>(note & 0x7Fu);
  // ~threefold growth per octave anchored near A4 (note 69), with a deep-bass
  // floor so the lowest wrapped strings keep a touch of stiffness.
  constexpr float kBAtA4 = 7.0e-4f;
  constexpr float kBetaPerSemitone = 0.0915750f;  // ln(3) / 12
  const float b = kBAtA4 * std::exp(kBetaPerSemitone * (n - 69.0f));
  return std::max(b, 2.0e-5f);
}

int piano_unison_strings(uint8_t note) noexcept {
  const int n = static_cast<int>(note & 0x7Fu);
  if (n <= 29) return 1;  // A0..F1: single wound string.
  if (n <= 47) return 2;  // F#1..B2: wound bichords.
  return 3;               // tenor break up: plain trichords.
}

float piano_stretch_cents(uint8_t note) noexcept {
  // Gentle odd cubic through A4 (note 69), clamped to a tasteful range. Real
  // Railsback curves steepen at the extremes; the clamp stands in for that
  // without overshooting into an audibly detuned keyboard.
  const float x = (static_cast<float>(note & 0x7Fu) - 69.0f) / 39.0f;
  return std::clamp(14.0f * x * x * x, -22.0f, 22.0f);
}

void PianoVoiceCore::start(const PianoPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed, bool una_corda) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Stretch tuning widens the octaves so the inharmonic partials lock the way
  // a tuned grand's do (sharp treble, flat bass; A4 anchored).
  const float f0 = note_to_hz(note) * std::exp2(piano_stretch_cents(note) / 1200.0f);
  const float period = static_cast<float>(sr) / f0;
  const float w0 = kTwoPi / period;
  VoiceRandomSequence jitter(seed);

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
  const float lp_a = (1.0f - bright_eff) * 0.6f;
  loop_alpha_ = 1.0f - lp_a;
  const float tau_lp = onepole_phase_delay(lp_a, w0);

  // Stiffness dispersion: the per-note inharmonicity coefficient B drives a
  // first-order allpass cascade that stretches the partials sharp to
  // f_n = n*f0*sqrt(1 + B*n^2). The patch dispersion knob scales B
  // (0 = harmonic string).
  const float dispersion = std::clamp(params.dispersion, 0.0f, 1.0f);
  const float b_coeff = piano_inharmonicity_b(note) * dispersion;
  const float phase_budget = period - 4.0f - tau_lp;
  const float ap_a = dispersion_allpass_a(b_coeff, w0, lp_a, kPianoDispersionStages, phase_budget);

  // Two-stage decay rates (stretched down the keyboard). The treble taper
  // shortens only the aftersound; the prompt stage instead blends toward the
  // aftersound rate away from the mid-range (see kTwoStageWidthOct).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float bass_scale = std::exp2(stretch * octaves_below_a4);
  const float slow_scale = bass_scale * std::exp2(-kTrebleDecayOct * octaves_above_c4);
  const float t60_slow =
      std::max(0.05f, std::max(params.decay_fast_s, params.decay_slow_s) * slow_scale);
  const float oct_from_c4_signed = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contrast = std::exp(-(oct_from_c4_signed * oct_from_c4_signed) /
                                  (kTwoStageWidthOct * kTwoStageWidthOct));
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
    s.size = std::min(string_capacity_, static_cast<int>(s.base_period * 1.3f) + 8);
    if (s.buffer != nullptr && s.size > 0) {
      std::fill(s.buffer, s.buffer + static_cast<size_t>(s.size), 0.0f);
    }
  }
  for (int i = num_strings_; i < kMaxPianoStrings; ++i) strings_[static_cast<size_t>(i)] = String{};
  bridge_ = 0.0f;
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
  release_gain_ =
      loop_gain_for(period, sr, std::max(0.01f, params.release_damp_s * damper_keytrack));

  // Dynamic felt hammer (F = k * x^p with hysteretic loss), integrated per
  // sample against the string at the strike point. The felt stiffness k is
  // calibrated so a mezzo-forte blow lands the reference contact time for the
  // register; from there the Hertz velocity laws (harder+shorter with faster
  // blows), the treble's long full-period dwell and the bass re-contact
  // chatter all EMERGE from the interaction instead of being prescribed.
  const float vel01 = std::max(static_cast<float>(velocity & 0x7Fu) / 127.0f, 0.02f);
  const float p = std::clamp(params.hammer_exponent, 1.5f, 4.0f);
  const float amp_exp = 2.0f * p / (p + 1.0f);
  const float dyn =
      std::clamp(params.hammer_dynamics, 0.0f, 1.0f) * (una_corda ? kUnaCordaDynScale : 1.0f);
  // Reference contact time for the register (mf): the patch contact scaled by
  // register, floored in fundamental PERIODS (treble dwell ~ a full period).
  float contact_ms = std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                     std::exp2(-(static_cast<float>(note & 0x7Fu) - 69.0f) / 13.1212f);
  const float octaves_from_c4 = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contact_floor_periods = std::clamp(
      kContactPeriodsAtC4 + kContactPeriodsPerOct * octaves_from_c4, 0.0f, kContactPeriodsMax);
  contact_ms =
      std::max(contact_ms, contact_floor_periods * 1000.0f * period / static_cast<float>(sr));
  const float tau_mf = std::max(8.0f, contact_ms * 0.001f * static_cast<float>(sr));
  // Free bounce of a unit mass on F = k * x^p from unit velocity lasts
  // c(p) * k^(-1/(p+1)) samples; c(p) fitted over p in [1.5, 4].
  const float c_p = 3.28f - 0.066f * p;
  // The una-corda felt patch is softer (lower k -> longer, darker contact).
  ham_p_ = p;
  ham_k_ = std::pow(c_p / tau_mf, p + 1.0f) * (una_corda ? 0.5f : 1.0f);
  // Felt hysteresis: loading is stiffer than unloading, which skews the force
  // pulse forward and bleeds energy so the hammer leaves the string cleanly.
  ham_mu_ = 0.229431f;
  ham_y_ = 0.0f;
  // Hammer speed normalized at the mezzo-forte reference; hammer_dynamics
  // widens the pp<->ff speed spread around that pivot.
  ham_v_ = std::pow(vel01 / kHammerMfVel, 1.0f + 0.6f * dyn);
  ham_on_ = true;
  ham_ttl_ = static_cast<int>(3.0f * tau_mf);  // shank check truncates a riding hammer
  // Peak force of that mf bounce (unit mass): k * x_max^p with
  // x_max = ((p+1)/2)^(1/(p+1)) * k^(-1/(p+1)); normalize the injection so
  // the mf level matches the classic voicing, with the velocity level curve
  // (~ v^(2p/(p+1))) emerging from the dynamics.
  const float x_max_mf = std::pow(0.5f * (p + 1.0f) / ham_k_, 1.0f / (p + 1.0f));
  const float f_peak_mf = ham_k_ * std::pow(x_max_mf, p);
  // Level reference: velocity-scaled for the noise/knock paths (as before);
  // the injection normalizes to the MF level so the velocity LEVEL curve
  // (~ v^(2p/(p+1))) comes out of the dynamics, not out of this constant.
  hammer_amp_ = 0.9f * std::pow(vel01, amp_exp) * (una_corda ? 0.8f : 1.0f);
  const float mf_level = 0.9f * std::pow(kHammerMfVel, amp_exp) * (una_corda ? 0.8f : 1.0f);
  ham_force_norm_ = f_peak_mf > 1.0e-12f ? mf_level / f_peak_mf : 0.0f;
  ham_force_norm_ *=
      std::exp2(kInjTiltDbOct * std::clamp(octaves_from_c4, -1.25f, 1.25f) / 6.0206f);
  // String yield under the blow: the strike point recedes with a velocity
  // proportional to the net force through the string's wave admittance, and
  // the inverted reflection from the NEAR end (agraffe side, back after
  // 2 * strike_position * period = comb_delay_ samples) cancels that recess
  // — the felt is recompressed and the measured multi-hump piano force
  // curve (and the treble's full-period dwell) emerges.
  const float yield_kt =
      kStringYield * std::exp2(-kYieldTrebleOct * std::max(0.0f, octaves_from_c4));
  ys_adm_ = 0.5f * yield_kt * x_max_mf;
  ys_ = 0.0f;
  last_force_ = 0.0f;
  ham_exit_ = -x_max_mf;
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
  const float exc_cutoff =
      800.0f * std::exp2(3.0f * vel01) * dyn_bright * (una_corda ? 0.4f : 1.0f);
  const float width_harm =
      kHammerWidthHarmonics * std::exp2(kWidthBassOct * std::max(0.0f, -octaves_from_c4) +
                                        kWidthTrebleOct * std::max(0.0f, octaves_from_c4));
  const float width_cutoff = std::min(exc_cutoff, width_harm * f0 * dyn_bright);
  exc_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * width_cutoff / static_cast<float>(sr)), 0.01f, 1.0f);
  // The noise cutoff keytracks DOWN into the bass: the bass hammer is a
  // massive deep-felt head whose scrub spectrum is far darker than the small
  // hard treble hammer's — without this the bass attack carries the same
  // 2 kHz-wide burst as the treble and reads as a jack click.
  const float noise_cutoff = kStrikeNoiseCutoffScale * exc_cutoff *
                             std::exp2(-kNoiseCutoffBassOct * std::max(0.0f, -octaves_from_c4));
  noise_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * noise_cutoff / static_cast<float>(sr)), 0.01f, 1.0f);
  noise_alpha3_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kNoiseSteepRatio * noise_cutoff / static_cast<float>(sr)), 0.01f,
      1.0f);
  exc_lp_ = 0.0f;
  exc_lp2_ = 0.0f;

  // Felt impact noise: a short broadband burst radiated with the knock (the
  // soft una-corda felt lands with far less impact noise than the grooved
  // normale surface).
  // The impact noise is part of the same blow — it rides the injection tilt
  // and the dynamics-gated felt compression (a compressed crown scrubs
  // louder, a soft blow on open felt barely rustles).
  noise_env_ = kStrikeNoiseGain * hammer_amp_ * dyn_bright * (una_corda ? 0.35f : 1.0f) *
               std::exp2(-kNoiseTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                         kInjTiltDbOct * std::clamp(octaves_from_c4, -1.25f, 1.25f) / 6.0206f);
  knock_gain_ = kKnockGain * std::exp2(kKnockBassBoostOct * std::max(0.0f, -octaves_from_c4) -
                                       kKnockTrebleTaperOct * std::max(0.0f, octaves_from_c4));
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
  // Below C4 the injection GROWS instead: the massive bass hammer's felt
  // scrub and re-strike chatter seed the dense h8-h20 partial cloud a wound
  // string radiates (absent it, the bass is a clean plucked stack).
  noise_inject_ =
      kStrikeNoiseInject * std::exp2(-kInjectTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                                     kInjectBassBoostOct * std::max(0.0f, -octaves_from_c4));

  // Radiation highpass coefficients (RBJ biquad) and state.
  {
    const float w = kTwoPi * kRadiationHpHz / static_cast<float>(sr);
    const float cw = std::cos(w);
    const float alpha = std::sin(w) / (2.0f * kRadiationHpQ);
    const float a0 = 1.0f + alpha;
    hp_b0_ = (1.0f + cw) * 0.5f / a0;
    hp_b1_ = -(1.0f + cw) / a0;
    hp_a1_ = -2.0f * cw / a0;
    hp_a2_ = (1.0f - alpha) / a0;
    hp_x1_ = hp_x2_ = hp_y1_ = hp_y2_ = 0.0f;
  }

  // Bridge-hill emphasis coefficients (RBJ peaking) and state.
  {
    const float big_a = std::pow(10.0f, kBridgeHillGainDb / 40.0f);
    const float w = kTwoPi * kBridgeHillHz / static_cast<float>(sr);
    const float cw = std::cos(w);
    const float alpha = std::sin(w) / (2.0f * kBridgeHillQ);
    const float a0 = 1.0f + alpha / big_a;
    bh_b0_ = (1.0f + alpha * big_a) / a0;
    bh_b1_ = -2.0f * cw / a0;
    bh_b2_ = (1.0f - alpha * big_a) / a0;
    bh_a1_ = -2.0f * cw / a0;
    bh_a2_ = (1.0f - alpha / big_a) / a0;
    bh_x1_ = bh_x2_ = bh_y1_ = bh_y2_ = 0.0f;
  }
}

float PianoVoiceCore::render(float pitch_ratio) noexcept {
  if (num_strings_ <= 0 || slab_ == nullptr) return 0.0f;

  // Dynamic hammer: integrate the felt mass against the string's arrival at
  // the strike point, then comb the force by the strike position and pass the
  // velocity-driven felt-stiffness lowpass.
  float exc = 0.0f;
  float knock = 0.0f;
  float thud_in = 0.0f;
  float force = 0.0f;
  if (ham_on_) {
    // String surface velocity at the strike point: the string recedes under
    // the net force through its wave admittance (the soft-string effect that
    // stretches the treble dwell and caps the energy transfer). The near-end
    // reflection is applied to the INJECTED wave by the strike-position comb
    // below; feeding it back into the felt as well would add energy the loop
    // never gave up (that needs a true bidirectional waveguide).
    const float ys_vel = ys_adm_ * last_force_;
    // Tension bounds the excursion: the strike point is a sprung wave port,
    // not a free particle — without the cap the string outruns the hammer
    // and the bounce never completes (a glued, energyless contact).
    ys_ = std::min(ys_ + ys_vel, ham_exit_ * -0.7f);
    const float x = ham_y_ - ys_;
    if (x > 0.0f) {
      const float xdot = ham_v_ - ys_vel;
      force = ham_k_ * std::pow(x, ham_p_) * (1.0f + ham_mu_ * xdot);
      force = std::max(force, 0.0f);
      ham_v_ -= force;
    }
    ham_y_ += ham_v_;
    if ((x <= 0.0f && ham_v_ < 0.0f && ham_y_ - ys_ < ham_exit_) || --ham_ttl_ <= 0) {
      ham_on_ = false;  // thrown clear (or shank recovery timeout)
      comb_tail_ = comb_delay_;
    }
  }
  if (ham_on_ || comb_tail_ > 0) {
    if (!ham_on_) --comb_tail_;
    const float tap = comb_hist_[static_cast<size_t>(
        (comb_idx_ - comb_delay_ + kHammerCombCapacity) % kHammerCombCapacity)];
    comb_hist_[static_cast<size_t>(comb_idx_)] = force;
    comb_idx_ = (comb_idx_ + 1) % kHammerCombCapacity;
    const float combed = ham_force_norm_ * (force - tap);
    // Two-pole felt lowpass: the footprint is a spatial window over the
    // string, whose transmission falls ~12 dB/oct past the cap — a single
    // pole leaves the mid harmonics plectrum-bright at every register.
    exc_lp_ += exc_alpha_ * (combed - exc_lp_);
    exc_lp2_ += exc_alpha_ * (exc_lp_ - exc_lp2_);
    exc = exc_lp2_ / static_cast<float>(num_strings_);
    thud_in = exc_lp2_;
    last_force_ = force;
  }
  if (noise_pos_ < noise_samples_) {
    ++noise_pos_;
    noise_rng_ = noise_rng_ * 1664525u + 1013904223u;
    const float white = static_cast<float>(noise_rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
    // Two-pole felt-noise lowpass, and the noise radiates ONLY through the
    // knock's thud filter (below): a felt hammer puts almost nothing above a
    // few kHz into the air at normal dynamics. The previous one-pole noise
    // radiated raw left the 3-12 kHz attack octaves 20-45 dB hotter than the
    // reference — a plectrum/jack click the ear keys on as "plucked string"
    // no matter how accurate the sustain is.
    noise_lp_ += noise_alpha_ * (white - noise_lp_);
    noise_lp2_ += noise_alpha_ * (noise_lp_ - noise_lp2_);
    noise_lp3_ += noise_alpha3_ * (noise_lp2_ - noise_lp3_);
    const float noise = noise_env_ * noise_lp3_;
    noise_env_ *= noise_decay_;
    thud_in += noise;
    noise_low_ += noise_hp_a_ * (noise - noise_low_);
    // The scrub noise is generated AT the strike point, so it sees the same
    // near-end reflection as the force pulse: comb it by the strike position
    // (its own history — the force comb carries different units/lifetime).
    // Without this the noise fills in the comb's h8-region notch, the
    // reference bass ladder's signature dip.
    const float scrub = noise_inject_ * (noise - noise_low_);
    const size_t widx = static_cast<size_t>((noise_pos_ - 1) % kHammerCombCapacity);
    noise_hist_[widx] = scrub;
    const int64_t tap_i = noise_pos_ - 1 - comb_delay_;
    const float tap =
        tap_i >= 0 ? noise_hist_[static_cast<size_t>(tap_i % kHammerCombCapacity)] : 0.0f;
    exc += (scrub - tap) / static_cast<float>(num_strings_);
  }
  // Three-pole thud filter: the knock is a LOW-frequency body event; fewer
  // poles leak the excitation's top octaves into the radiated attack, and
  // the bass knock gain amplifies that leak into an audible jack click.
  knock_lp_ += knock_lp_a_ * (thud_in - knock_lp_);
  knock_lp2_ += knock_lp_a_ * (knock_lp_ - knock_lp2_);
  knock_lp3_ += knock_lp3_a_ * (knock_lp2_ - knock_lp3_);
  knock += knock_gain_ * knock_lp3_;

  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  float sum = 0.0f;
  float lp_sum = 0.0f;
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    if (s.buffer == nullptr || s.size < 8) continue;
    // Coupled two-stage decay: the coherent (bridge) component recirculates
    // at the fast prompt rate, the residual at the slow aftersound rate.
    const float fb = s.g_slow * s.lp_state - (s.g_slow - s.g_fast) * bridge_;
    const float delay =
        std::clamp(s.base_period / ratio - s.comp, 1.0f, static_cast<float>(s.size - 4));
    const float out = rt::lagrange3_fractional_delay(
        s.buffer, static_cast<size_t>(s.size), s.write_index, static_cast<int>(delay * 256.0f),
        exc * s.strike_weight + fb);
    // Dispersion allpass cascade then the loop lowpass.
    float v = out;
    for (float& state : s.ap_state) {
      const float y = s.ap_a * v + state;
      state = v - s.ap_a * y;
      v = y;
    }
    s.lp_state += loop_alpha_ * (v - s.lp_state);
    lp_sum += s.lp_state;
    sum += out * s.radiate_weight;
  }
  bridge_ = lp_sum / static_cast<float>(num_strings_);
  // Board ring-up: the tone swells while the impact thud leads.
  bloom_ += bloom_a_ * (1.0f - bloom_);
  sum = sum * bloom_ + knock;
  // Soundboard radiation: the board barely radiates the lowest partials.
  const float y =
      hp_b0_ * sum + hp_b1_ * hp_x1_ + hp_b0_ * hp_x2_ - hp_a1_ * hp_y1_ - hp_a2_ * hp_y2_;
  hp_x2_ = hp_x1_;
  hp_x1_ = sum;
  hp_y2_ = hp_y1_;
  hp_y1_ = y;
  // Bridge hill: the fixed-band mobility peak lifts whatever partials land
  // near it (bass crown, mid presence, treble body).
  const float z =
      bh_b0_ * y + bh_b1_ * bh_x1_ + bh_b2_ * bh_x2_ - bh_a1_ * bh_y1_ - bh_a2_ * bh_y2_;
  bh_x2_ = bh_x1_;
  bh_x1_ = y;
  bh_y2_ = bh_y1_;
  bh_y1_ = z;
  return z;
}

void PianoVoiceCore::release() noexcept {
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.g_slow = std::min(s.g_slow, release_gain_);
    s.g_fast = std::min(s.g_fast, release_gain_);
  }
}

void PianoVoiceCore::damp(float strength) noexcept {
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (strength <= 0.0f) return;
  if (strength >= 1.0f) {
    release();
    return;
  }
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.g_slow = std::min(s.g_slow, partial_damp_gain(s.g_slow, release_gain_, strength));
    s.g_fast = std::min(s.g_fast, partial_damp_gain(s.g_fast, release_gain_, strength));
  }
}

void PianoVoiceCore::kill() noexcept {
  for (String& s : strings_) s = String{};
  num_strings_ = 0;
  hammer_amp_ = 0.0f;
  ham_on_ = false;
  noise_pos_ = 0;
  noise_samples_ = 0;
  noise_env_ = 0.0f;
  noise_lp_ = 0.0f;
  noise_lp2_ = 0.0f;
  noise_lp3_ = 0.0f;
}

}  // namespace sonare::midi::synth
