#include "midi/synth/piano_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice_calibration.h"
#include "midi/synth/piano_voice_math.h"
#include "midi/synth/pitch.h"
#include "rt/biquad_design.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

/// Share of the strike noise that reaches the air directly, alongside the
/// share the board radiates through the knock's thud filter.
///
/// Sending all of it through the thud was a correction for raw noise leaving
/// the 3-12 kHz attack octaves 20-45 dB hot, and it overshot: the thud corner
/// keytracks to 72 Hz at A0, so in the bass the noise is lowpassed an octave
/// below the fundamental and radiates nothing at all. Measured against the
/// reference over the first 50 ms, the attack came out 10-18 dB short above
/// 3 kHz at EVERY register and 20 dB short at 0.8-3 kHz in the bass — a piano
/// whose hammers never land. The sustained partial ladder cannot see this;
/// it is measured after the attack is over.
///
/// The noise is a 30 ms event with an 8 ms decay, so this path touches the
/// attack only. What rings on is the scrub the strings are injected with,
/// which is kStrikeNoiseInject and is not affected by this.
SONARE_TUNABLE(kStrikeNoiseDirect, 0.6f);

/// How much of the knock is taken from the blow itself rather than from the
/// wave injected into the string. 0 keeps the injected copy, 1 takes the blow.
///
/// The knock is a STRUCTURE-BORNE path: the hammer landing shakes the action,
/// the frame and the rim, and that reaches the board without passing through
/// the string at all. It was nonetheless being tapped off the injected
/// excitation, which carries the strike-position comb — the near-end reflection
/// arriving back at the strike point, a property of the string the rim has no
/// way of knowing about.
///
/// The comb delay is a fixed FRACTION of the period, so what it does depends
/// entirely on the register. At C4 it is fifteen samples and its first null
/// sits at 1.6 kHz, well above the thud, which passes untouched. At C8 it is
/// ONE sample, which is a first difference: the comb becomes a differentiator
/// and takes 47 dB out at 100 Hz. That is why the model's low-band attack fell
/// 25 dB from A2 to C8 while the reference's is flat to within 5 -- the top of
/// the keyboard was not radiating a quieter body, it was radiating the
/// derivative of one.
///
/// It had been left off on a one-knob sweep, and that sweep was answering the
/// wrong question twice over.
///
/// The first error was the metric. Levels were compared with no gain alignment
/// at all, while the model's own held level ran a note-dependent 3 to 22 dB
/// over the reference, so a body level read as "too loud" that was in fact
/// deficient. Align exactly one scalar -- the mean held-level offset over the
/// keyboard, which is what a master gain can move and nothing else is -- and
/// the sign flips: switching the blow on and touching nothing else takes the
/// keyboard's low-band error from 65.3 dB to 39.8.
///
/// The second was the sweep itself. The comb's low-frequency loss goes as its
/// delay, a fixed fraction of the period, so it had been acting as an unnamed
/// register taper with the fitted gain sitting on top of it; moving the blow
/// alone measures that compensation and not the mechanism. Solved jointly with
/// the body, the envelope and the excitation it is the largest single term in
/// the voice: taking it back out costs 11.8 dB of total error against the
/// reference where the next knob costs 2.0.
///
/// The shape generalises past this knob. A mechanism the model lacks gets
/// stood in for by whatever knob is nearest, and the stand-in is then what a
/// one-at-a-time sweep is measuring.
SONARE_TUNABLE(kKnockUncombed, 1.0f);

}  // namespace

using namespace piano_detail;
using sonare::constants::kPi;

void PianoVoiceCore::refresh_modal_coefficients(float pitch_ratio) noexcept {
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  modal_cached_ratio_ = ratio;
  modal_active_count_ = 0;
  for (int i = 0; i < num_modal_; ++i) {
    ModalMode& m = modal_[static_cast<size_t>(i)];
    const float w = m.omega0 * ratio;
    if (m.r <= 0.0f || m.base_period <= 0.0f || w <= 0.0f || w >= 0.9f * kPi) {
      // Keep the descriptor for a later downbend; never resurrect out-of-band state.
      if (m.audible) {
        m.gain = 0.0f;
        m.a1 = 0.0f;
        m.a2 = 0.0f;
        m.y1 = 0.0f;
        m.y2 = 0.0f;
        m.audible = false;
      }
      continue;
    }
    const bool was_audible = m.audible;
    m.gain = piano_modal_drive_gain(m.weight, ratio, w, m.base_period);
    m.a1 = 2.0f * m.r * std::cos(w);
    if (!was_audible) m.a2 = -m.r * m.r;
    if (!was_audible) m.audible = true;
    modal_active_indices_[modal_active_count_++] = static_cast<uint8_t>(i);
  }
}

float PianoVoiceCore::render(float pitch_ratio) noexcept {
  if (num_strings_ <= 0 || slab_ == nullptr) return 0.0f;
  return decimator_.run([&] { return render_internal(pitch_ratio); });
}

float PianoVoiceCore::render_internal(float pitch_ratio) noexcept {
  if (num_strings_ <= 0 || slab_ == nullptr) return 0.0f;

  // Dynamic hammer: integrate the felt mass against the string's arrival at
  // the strike point, then comb the force by the strike position and pass the
  // velocity-driven felt-stiffness lowpass.
  float exc = 0.0f;
  float knock = 0.0f;
  float thud_in = 0.0f;
  float noise_direct = 0.0f;
  float force = 0.0f;
  if (ham_on_) {
    // String surface velocity at the strike point: the string recedes under
    // the net force through its wave admittance (the soft-string effect that
    // stretches the treble dwell and caps the energy transfer). The near-end
    // reflection is applied to the INJECTED wave by the strike-position comb
    // below; coupling it back into the felt as well — so the returning wave
    // pushes the string into the hammer and ends the contact on the string's
    // timescale rather than the felt's — measures as no change at all, in the
    // partial balance, in the pp<->ff spread, or in the treble dwell it would
    // most be expected to reach, so the one-way form stands.
    const float ys_vel = ys_adm_ * last_force_;
    // Tension bounds the excursion: the strike point is a sprung wave port,
    // not a free particle — without the cap the string outruns the hammer
    // and the bounce never completes (a glued, energyless contact). The bound
    // scales with the blow, because this blow's peak felt compression does.
    ys_ = std::min(ys_ + ys_vel, ys_limit_);
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
    // The body's copy of the same blow, softened by the same felt but never
    // combed (see kKnockUncombed). Written as a lerp off the combed path so
    // the transparent coefficient is exactly the value that path already had.
    const float raw = ham_force_norm_ * force;
    body_lp_ += exc_alpha_ * (raw - body_lp_);
    body_lp2_ += exc_alpha_ * (body_lp_ - body_lp2_);
    thud_in = exc_lp2_ + kKnockUncombed * (body_lp2_ - exc_lp2_);
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
    // The direct path is tapped two poles in, not three: the third pole belongs
    // to the contact footprint, which is what the STRING is injected through,
    // and the air does not hear the strike through the string's window. Tapped
    // at the same point the direct path carries nothing above a few hundred
    // hertz and cannot fill the attack it exists to fill.
    //
    // Two, not one. A single pole falls at 6 dB/octave, which no radiating
    // mechanism does and which leaves the burst still 23 dB up at 16 kHz —
    // exactly the plectrum click the cascade above was widened to two poles to
    // remove, arriving by the other route. Measured against the dry concert
    // grand a one-pole tap ran +32 dB at 12-20 kHz and +53 dB at 20-24 kHz over
    // the first 43 ms of every note, on top of a midband that already matched
    // inside half a dB; the ear reads that as a tick on the note, not as
    // brightness. Two poles put the same corner 45 dB down at 16 kHz against a
    // measured 44, and cost nothing — the tap already exists for the cascade.
    noise_direct = noise_env_ * noise_lp2_;
    noise_low_ += noise_hp_a_ * (noise - noise_low_);
    // The scrub noise is generated AT the strike point, so it sees the same
    // near-end reflection as the force pulse: comb it by the strike position
    // (its own history — the force comb carries different units/lifetime).
    // Without this the noise fills in the comb's h8-region notch, the
    // reference bass ladder's signature dip.
    float scrub = noise_inject_ * (noise - noise_low_);
    if (scrub_hf_gain_ != 1.0f) {
      scrub = scrub_hi_[1].process(scrub_hi_[0].process(scrub));
    }
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
  knock += knock_gain_ * knock_lp3_ + kStrikeNoiseDirect * noise_direct;

  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  if (ratio != modal_cached_ratio_) refresh_modal_coefficients(ratio);
  float sum = 0.0f;
  float lp_sum = 0.0f;
  // Fully past the crossover the loop contributes nothing, so it is not run:
  // the modal bank is meant to be the cheaper path up there, and leaving four
  // allpasses and an interpolator per string spinning behind a zero weight
  // would spend the saving it exists to make. `bridge_` then falls to zero on
  // its own, which is what a voice with no loop should hand the drain.
  const int loop_strings = modal_mix_ < 1.0f ? num_strings_ : 0;
  for (int i = 0; i < loop_strings; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    if (s.buffer == nullptr || string_capacity_ < 8) continue;
    // Coupled two-stage decay: the coherent (bridge) component recirculates
    // at the fast prompt rate, the residual at the slow aftersound rate.
    const float fb = s.g_slow * s.lp_state - (s.g_slow - s.g_fast) * drain_out_;
    const float delay =
        std::clamp(s.base_period / ratio - s.comp, 1.0f, static_cast<float>(string_capacity_ - 4));
    const float out = rt::lagrange3_fractional_delay(
        s.buffer, static_cast<size_t>(string_capacity_), s.write_index,
        static_cast<int>(delay * 256.0f), exc * s.strike_weight + fb);
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
  // Band-limited copy for the drain only. `bridge_` itself stays broadband:
  // it is the coherent bridge signal and other things read it as such.
  //
  // Written as a lag off the input rather than as the usual `y += a * (x - y)`,
  // which at a == 1 is `y + (x - y)` and is NOT x in floating point. The two
  // differ in the last bit, and one last bit inside a feedback loop running
  // four thousand times a second is not a rounding difference for long. This
  // form is exactly `bridge_` at the transparent coefficient.
  bridge_drain_ = bridge_ - (1.0f - drain_lp_a_) * (bridge_ - bridge_drain_);
  // The upper band. Written so that a zero weight leaves `drain_out_` exactly
  // `bridge_drain_` -- the one-pole still runs, but nothing it produces reaches
  // the loop, so the voice renders bit-identically to the band-limited drain
  // alone.
  bridge_hf_lp_ += drain_hf_a_ * (bridge_ - bridge_hf_lp_);
  drain_out_ = bridge_drain_ + drain_hi_w_ * (bridge_ - bridge_hf_lp_);
  // Modal top-octave bank: the same hammer excitation, resolved into partials.
  // It stands in for the string's radiated wave and nothing else -- knock,
  // scrub noise, longitudinal bank, board and bridge hill all read the blended
  // `sum` exactly as they read the loop's.
  if (num_modal_ > 0) {
    float modal_sum = 0.0f;
    for (uint8_t i = 0; i < modal_active_count_; ++i) {
      ModalMode& m = modal_[modal_active_indices_[i]];
      const float y = m.gain * exc + m.a1 * m.y1 + m.a2 * m.y2;
      m.y2 = m.y1;
      m.y1 = y;
      modal_sum += y;
    }
    modal_env_ *= modal_damp_;
    float modal_gain = modal_env_;
    // Tested rather than folded in, for the same reason the drain's weight is:
    // at a residue of 1 the factor below is `1 + 0 * prompt`, which is 1 in
    // floating point too, but the state update behind it is not free and there
    // is nothing for it to do.
    if (modal_residue_ != 1.0f) {
      modal_prompt_ *= modal_prompt_r_;
      modal_gain *= modal_residue_ + (1.0f - modal_residue_) * modal_prompt_;
    }
    sum += (modal_sum * modal_gain - sum) * modal_mix_;
  }
  // Board ring-up: the tone swells while the impact thud leads.
  bloom_ += bloom_a_ * (1.0f - bloom_);
  // Longitudinal modes, driven by the tension the transverse motion itself
  // makes. Squaring the string sum IS the tension term, so the v^2 amplitude
  // law and the doubled decay rate come out of it rather than being written
  // down; the two-sample difference puts a zero at DC and at Nyquist, without
  // which the squared drive's large DC component would be re-radiated as the
  // very bass rumble this bank is here to replace.
  float longitudinal = 0.0f;
  if (long_level_ > 0.0f) {
    // The tension follows the string's SLOPE, not its displacement, so the
    // drive is differenced before it is squared. It matters for the envelope
    // rather than the spectrum: the slope is carried by the upper transverse
    // partials, which die first, so a differenced drive concentrates the bank
    // in the attack the way a real phantom partial is. Squaring the radiated
    // sum instead keeps the bank ringing for as long as the fundamental does,
    // which measured 13 points brighter than the reference over the sustain
    // at the level the attack needs.
    long_prev_ += long_hp_a_ * (sum - long_prev_);
    const float d = sum - long_prev_;
    const float t = d * d;
    const float bp = t - long_x2_;
    long_x2_ = long_x1_;
    long_x1_ = t;
    for (LongMode& m : long_modes_) {
      const float y = m.gain * bp + m.a1 * m.y1 + m.a2 * m.y2;
      m.y2 = m.y1;
      m.y1 = y;
      longitudinal += y;
    }
    longitudinal *= long_level_;
  }
  sum = sum * bloom_ + knock + longitudinal;
  // Soundboard radiation: the board barely radiates the lowest partials.
  float y = sum;
  for (HpSection& s : hp_) {
    const float in = y;
    y = s.b0 * in + s.b1 * s.x1 + s.b0 * s.x2 - s.a1 * s.y1 - s.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = in;
    s.y2 = s.y1;
    s.y1 = y;
  }
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
  // The modal bank's damper multiplies the modes' own decay instead of
  // capping it. Decay RATES add when a second loss mechanism is introduced,
  // which is what a damper is; the loop's min() is a cap only because a
  // per-traversal gain has no term to add to.
  modal_damp_ = std::min(modal_damp_, modal_release_);
}

void PianoVoiceCore::damp(float strength) noexcept {
  strength = std::clamp(strength, 0.0f, 1.0f);
  // Contact position, not an event: recompute loss from the natural and full endpoints.
  if (strength <= 0.0f) {
    for (int i = 0; i < num_strings_; ++i) {
      String& s = strings_[static_cast<size_t>(i)];
      s.g_slow = s.g_slow_natural;
      s.g_fast = s.g_fast_natural;
    }
    modal_damp_ = 1.0f;
    return;
  }
  if (strength >= 1.0f) {
    release();
    return;
  }
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    const float slow_damped = std::min(s.g_slow_natural, release_gain_);
    const float fast_damped = std::min(s.g_fast_natural, release_gain_);
    s.g_slow = partial_damp_gain(s.g_slow_natural, slow_damped, strength);
    s.g_fast = partial_damp_gain(s.g_fast_natural, fast_damped, strength);
  }
  // Partial contact scales the ADDED rate, so a half-resting damper removes
  // half the energy per second the full one does. partial_damp_gain's
  // geometric rule cannot be reused here: it interpolates between two finite
  // t60s, and the modal bank's undamped end is an infinite one.
  modal_damp_ = std::pow(modal_release_, strength);
}

void PianoVoiceCore::kill() noexcept {
  decimator_.reset();
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
  body_lp_ = 0.0f;
  body_lp2_ = 0.0f;
  long_level_ = 0.0f;
  long_prev_ = 0.0f;
  long_x1_ = 0.0f;
  long_x2_ = 0.0f;
  for (LongMode& m : long_modes_) m = LongMode{};
  num_modal_ = 0;
  modal_active_count_ = 0;
  modal_mix_ = 0.0f;
  modal_env_ = 1.0f;
  modal_damp_ = 1.0f;
  modal_prompt_ = 1.0f;
  modal_prompt_r_ = 1.0f;
  modal_residue_ = 1.0f;
  modal_cached_ratio_ = 1.0f;
}

}  // namespace sonare::midi::synth
