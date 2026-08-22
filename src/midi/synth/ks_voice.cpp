#include "midi/synth/ks_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "rt/dispersion.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

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
  return std::max(1.0e-5f, kBAtA4 * std::exp(kBetaPerSemitone * (n - 69.0f)));
}

/// Second (horizontal) polarization detune, and the fret-gap reflection left
/// after the slap/pop limiter clips the string against the fret.
SONARE_TUNABLE(kPolDetuneCents, 11.0f);
SONARE_TUNABLE(kReflect, 0.06f);  // near-hard clip at the fret gap

/// KS noise draws live far above the voice-level draw indices (detune/phase/
/// drift use 0..~103 on the same per-voice seed).
constexpr uint64_t kNoiseIndexBase = 1ull << 16;
/// Key-off damper-noise draws sit well above the excitation burst so the two
/// seeded streams never overlap.
constexpr uint64_t kKeyoffNoiseIndexBase = 1ull << 20;
/// Key-off damper thump: burst length and lowpass corner (a soft felt "thunk").
SONARE_TUNABLE(kKsKeyoffMs, 18.0f);
SONARE_TUNABLE(kKsKeyoffCutoffHz, 2200.0f);

}  // namespace

void KsVoiceCore::start(const KsPatchParams& params, double sample_rate, uint8_t note,
                        uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);

  const float f0 = note_to_hz(note);
  const float base_period = static_cast<float>(sr) / f0;

  // Loop lowpass: brightness -> feedback coefficient a (y += (1-a)(x-y)).
  const float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f;

  // Decay: t60 stretched per octave below A4 (low strings ring longer).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);
  const float damped_t60 = std::max(0.01f, params.release_damp_s);

  // The played string: the vertical plane the pluck grips. configure() tunes it
  // by compensating the EXACT phase delay of the loop filter at the fundamental
  // (not just its DC group delay) plus the one-sample feedback path, so the
  // sounding pitch matches the note to a few cents.
  string_.configure(slab_, capacity_, base_period, sr, a, t60, damped_t60);

  // Stiff-string dispersion (steel strings). 0 disables the allpass cascade so
  // the loop stays a harmonic string, bit-identical. Otherwise scale the steel
  // inharmonicity into an in-loop allpass (the shared piano dispersion solver)
  // and fold its phase delay into the loop compensation so f0 tuning holds.
  const float omega = kTwoPi / base_period;
  const float tau_lp = onepole_group_delay_samples(a, omega);
  disp_a_ = 0.0f;
  for (float& s : disp_state_) s = 0.0f;
  const float dispersion = std::clamp(params.dispersion, 0.0f, 1.0f);
  if (dispersion > 0.0f) {
    const float b_coeff = dispersion * ks_steel_inharmonicity_b(note);
    const float phase_budget = base_period - 4.0f - tau_lp;
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
  pick_delay_ = static_cast<int>(std::clamp(params.pick_position, 0.0f, 0.5f) * base_period + 0.5f);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float bright =
      std::clamp(params.exc_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);
  // Exponential brightness -> cutoff map (300 Hz .. ~12 kHz) through two
  // cascaded one-poles, so the velocity swing is clearly audible.
  const float exc_cutoff = 300.0f * std::exp2(5.3f * bright);
  exc_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * exc_cutoff / static_cast<float>(sr)), 0.01f, 1.0f);
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

  // Magnetic pickup (electric). 0 disables it so the output stays bit-identical.
  // The pickup senses the string at a fixed point: a second, output-side comb
  // with a node at pickup_pos of the period, plus a mild field-gradient
  // nonlinearity (even harmonics). The tap is at least a few samples so the
  // read-only loop tap never lands on the pending write.
  const float pickup = std::clamp(params.pickup_pos, 0.0f, 0.5f);
  if (pickup > 0.0f) {
    // The loop output tap sits ~a full period back, so tapping (1 - pickup) of
    // the period makes the comb delay between the two taps pickup * period: a
    // pickup near the bridge (small pickup) combs at a short delay (its first
    // peak high = bright), a neck pickup at a longer delay (rounder).
    const float offset = std::clamp((1.0f - pickup) * base_period, 4.0f, base_period);
    pickup_delay_q8_ = static_cast<int>(offset * 256.0f);
    pickup_depth_ = 0.85f;  // near-full comb notch depth
    pickup_mag_ = 0.18f;    // gentle even-harmonic nonlinearity
  } else {
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
    tension_ratio_peak_ = std::exp2(rise_cents / 1200.0f) - 1.0f;
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
    const float pol_period = base_period / std::exp2(kPolDetuneCents / 1200.0f);
    // A darker loop filter: the horizontal plane loses its highs faster, and a
    // faster decay (a fraction of the primary t60) gives the two-stage decay.
    const float a2 = std::min(0.97f, a + 0.12f);
    pol_.configure(slab_ + capacity_, capacity_, pol_period, sr, a2, 0.55f * t60, damped_t60);
    // The damper grips both planes at once, so the horizontal one is released at
    // the played string's damped gain rather than at one solved for its own
    // (slightly shorter) period.
    pol_.release_gain = string_.release_gain;
    pol_exc_ = 0.6f;  // the pluck grips the vertical plane; the horizontal weakly
    pol_couple_ = polarization;

    // Bridge coupling (off unless body_coupling > 0). The two loops close a
    // symmetric 2x2 system [[g1, eps], [eps, g2]] once they exchange energy
    // through the bridge; its spectral radius is max_eig = mean + sqrt(halfdiff^2
    // + eps^2), NOT max(g1, g2). Near the degenerate detune (g1 ~= g2) even a
    // small eps can push a plane over unity, so solve for the largest eps that
    // keeps max_eig <= kLambdaMax and scale body_coupling within that envelope.
    const float bc = std::clamp(params.body_coupling, 0.0f, 1.0f);
    if (bc > 0.0f) {
      constexpr float kLambdaMax = 0.999f;
      const float mean = 0.5f * (string_.gain + pol_.gain);
      const float half_diff = 0.5f * (string_.gain - pol_.gain);
      const float room = kLambdaMax - mean;
      float eps_max = 0.0f;
      if (room > 0.0f) {
        const float r2 = room * room - half_diff * half_diff;
        if (r2 > 0.0f) eps_max = std::sqrt(r2);
      }
      couple_gain_ = bc * eps_max;
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
    // Same loop brightness as the primary; configure() recomputes the phase-delay
    // compensation at the octave-up fundamental so the 4' pitch is accurate.
    oct_.configure(slab_ + 2 * capacity_, capacity_, 0.5f * base_period, sr, a, t60, damped_t60);
    oct_exc_ = 0.7f;  // the 4' jack grips its string a touch less than the 8'
    oct_couple_ = octave_mix;
  } else {
    oct_.disable();
    oct_couple_ = 0.0f;
  }

  // Key-off / damper noise. 0 disables it (release() never arms the burst, output
  // bit-identical). Precompute the burst length and its lowpass corner; the burst
  // itself is triggered at note-off.
  keyoff_amount_ = std::clamp(params.keyoff_noise, 0.0f, 1.0f);
  keyoff_len_ = std::max(1, static_cast<int>(kKsKeyoffMs * 0.001f * static_cast<float>(sr)));
  keyoff_pos_ = keyoff_len_;  // inactive until release()
  keyoff_lp_ = 0.0f;
  keyoff_env_ = 0.0f;
  keyoff_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * kKsKeyoffCutoffHz / static_cast<float>(sr)),
                             0.01f, 1.0f);
  keyoff_decay_ = std::exp(-4.0f / static_cast<float>(keyoff_len_));  // ~-35 dB over the burst
}

float KsVoiceCore::render(float pitch_ratio) noexcept {
  if (string_.buffer == nullptr || string_.size < 8) return 0.0f;

  float exc = 0.0f;
  if (exc_pos_ < exc_total_ + pick_delay_) {
    // Excitation source: the seeded noise burst, optionally crossfaded toward a
    // deterministic pluck doublet. pluck_style_ == 0 returns the raw noise so the
    // path is bit-identical.
    auto source_at = [this](int k) noexcept -> float {
      const float nz = noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(k));
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
    };
    // Pick-position comb: burst[n] - burst[n - pick_delay]. The delayed copy
    // runs pick_delay samples past the burst so the comb notches stay exact.
    float burst = exc_pos_ < exc_total_ ? source_at(exc_pos_) : 0.0f;
    if (pick_delay_ > 0 && exc_pos_ >= pick_delay_) {
      burst -= source_at(exc_pos_ - pick_delay_);
    }
    ++exc_pos_;
    exc_lp1_ += exc_alpha_ * (burst - exc_lp1_);
    exc_lp2_ += exc_alpha_ * (exc_lp1_ - exc_lp2_);
    exc = 0.7f * exc_lp2_;  // comb headroom
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
    pickup_tap = rt::lagrange3_read(string_.buffer, static_cast<size_t>(string_.size),
                                    string_.write, pickup_delay_q8_);
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

  if (keyoff_pos_ < keyoff_len_) {
    // Key-off damper thump: a short lowpassed, decaying noise burst armed at
    // note-off (skipped entirely when keyoff_amount_ == 0 -> bit-identical).
    const float nz = noise_.bipolar_at(kKeyoffNoiseIndexBase + static_cast<uint64_t>(keyoff_pos_));
    keyoff_lp_ += keyoff_alpha_ * (nz - keyoff_lp_);
    result += keyoff_amount_ * keyoff_env_ * keyoff_lp_;
    keyoff_env_ *= keyoff_decay_;
    ++keyoff_pos_;
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
  if (keyoff_amount_ > 0.0f) {
    keyoff_pos_ = 0;
    keyoff_lp_ = 0.0f;
    keyoff_env_ = 1.0f;
  }
}

void KsVoiceCore::kill() noexcept {
  exc_pos_ = exc_total_;
  string_.kill();
  pol_.kill();
  oct_.kill();
  keyoff_pos_ = keyoff_len_;
}

}  // namespace sonare::midi::synth
