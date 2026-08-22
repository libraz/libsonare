#include "midi/synth/bowed_string_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kPi;
using sonare::constants::kTwoPi;

// Bow-velocity calibration: the differential-velocity input the bow table sees
// is a small quantity (a real bow moves the contact point at ~0.05-0.2 in the
// model's normalised velocity-wave units). maxVelocity = base + span*speed.
SONARE_TUNABLE(kBowVelocityBase, 0.03f);
SONARE_TUNABLE(kBowVelocitySpan, 0.14f);

// Rosin texture depth: a light seeded jitter on the bow velocity.
SONARE_TUNABLE(kRosinDepth, 0.15f);

// Live-control smoothing time (ms): the per-sample ramp of bow speed / force /
// position toward their CC targets — fast enough to feel immediate, slow enough
// to never zipper.
SONARE_TUNABLE(kControlSmoothMs, 8.0f);

// --- elasto-plastic friction calibration (only when params.elasto_plastic) ---
// All dimensionless in the model's velocity-wave units (bow velocities ~0.03..
// 0.17); tuned so the bristle loop stays bounded and still locks into Helmholtz.
// Stribeck velocity (stick-hump half-width) = base + span*stribeck: the relative
// velocity at which the bristles break away from full grip into slip.
SONARE_TUNABLE(kEpStribeckBase, 0.02f);
SONARE_TUNABLE(kEpStribeckSpan, 0.10f);
// Bristle deflection clamp (divergence guard rail) and the breakaway fraction of
// it below which the contact is purely elastic (stuck, no plastic slip yet).
SONARE_TUNABLE(kEpZMax, 0.25f);
SONARE_TUNABLE(kEpBreakawayFrac, 0.15f);
// Bristle load time (ms): how quickly the stuck bristle charges toward its
// steady deflection; folded into a per-sample rate against the sample rate. Kept
// well under a fundamental period so the stick->slip corner stays sharp (a slow
// bristle over-smooths the tone into a near-sine).
SONARE_TUNABLE(kEpLoadTimeMs, 0.15f);
// Steady-state bristle floor (fraction of z_max the hump retains in full slip),
// so the Stribeck curve decays to a small non-zero grip rather than to zero.
SONARE_TUNABLE(kEpZssFloor, 0.10f);
// Hysteresis bias: how strongly the bristle deflection offsets the (sharp) bow
// table's operating point. This is the whole elasto-plastic effect — the table
// stays sharp (Helmholtz preserved) but breaks away and re-grips along different
// velocities, the hysteresis loop that warms the memoryless table's "dry" slip.
SONARE_TUNABLE(kEpHystOffset, 0.6f);

// --- sympathetic open-string resonance (only when params.sympathetic > 0) ---
// Fixed open-string pitches (MIDI notes) the bank resonates at: a fifths-ish
// spread spanning the violin family's open strings (contrabass E1 .. violin E5),
// so a bowed note's partials excite whichever "open strings" they align with.
constexpr int kSympatheticNotes[] = {28, 35, 42, 49, 55, 62, 69, 76};
// Sympathetic ring time (t60, seconds): open strings ring longer than the
// piano's damped bank but decay well within a phrase.
SONARE_TUNABLE(kSympatheticRingS, 1.2f);
// Return level scale: weak coupling so the bank haloes the played note rather
// than dominating (final mix = this * params.sympathetic).
SONARE_TUNABLE(kSympatheticOutGain, 0.10f);

// --- second (horizontal) polarization (only when params.polarization > 0) ---
// Detune of the 2nd polarization from the bowed one (cents): the two planes ring
// at slightly different pitches and beat, the source of the "thickness".
SONARE_TUNABLE(kPolDetuneCents, 7.0f);
// Loop loss of the 2nd polarization: more damped than the primary (it is not
// driven directly by the bow), so it colours the attack and body without ringing
// on forever.
SONARE_TUNABLE(kPolLoss, 0.93f);
// Reflection-filter openness of the 2nd polarization loop (one-pole pole a).
SONARE_TUNABLE(kPolLpPole, 0.35f);
// Bow injection into the 2nd polarization (weak — the bow grips the vertical
// plane; the horizontal plane is dragged along).
SONARE_TUNABLE(kPolDrive, 0.35f);
// Cross-coupling of the 2nd polarization back into the bowed string velocity,
// scaled by params.polarization. Kept small so the added feedback path stays
// bounded (the bow table saturates and pol_loss < 1).
SONARE_TUNABLE(kPolCoupleMax, 0.20f);
// Direct radiation of the 2nd polarization added to the output.
SONARE_TUNABLE(kPolRadiation, 0.25f);

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

}  // namespace

void BowedStringVoiceCore::start(const BowedStringPatchParams& params, double sample_rate,
                                 uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;
  releasing_ = false;
  bow_level_ = 0.0f;
  lp_state_ = 0.0f;
  neck_out_ = 0.0f;
  bridge_out_ = 0.0f;

  const float f0 = note_to_hz(note);
  base_period_ = static_cast<float>(sr) / std::max(1.0f, f0);
  beta_ = std::clamp(params.bow_position, 0.02f, 0.5f);
  beta_target_ = beta_;

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  // Bow speed = the dynamic level: the patch speed blended with the struck
  // velocity by vel_to_speed. This is the note-on base the expression scales.
  const float vel_to_speed = std::clamp(params.vel_to_speed, 0.0f, 1.0f);
  const float speed =
      std::clamp((1.0f - vel_to_speed) * params.bow_speed + vel_to_speed * vel01, 0.0f, 1.0f);
  base_bow_velocity_ = kBowVelocityBase + kBowVelocitySpan * speed;
  max_bow_velocity_ = base_bow_velocity_;
  bow_speed_target_ = base_bow_velocity_;

  // Bow force -> friction-curve slope (wider sticking region at higher force).
  const float force = std::clamp(params.bow_force, 0.0f, 1.0f);
  bow_slope_ = kBowSlopeMax_ - kBowSlopeSpan_ * force;
  slope_target_ = bow_slope_;
  bow_offset_ = 0.0f;

  // Live-control smoothing coefficient (per-sample one-pole toward the targets).
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);

  // Bridge loop lowpass: brightness -> pole a (y += (1-a)(x - y)). A brighter
  // bridge reflects more upper partials (edgier tone).
  const float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f;
  lp_alpha_ = 1.0f - a;
  // String damping -> bridge loss gain (< 1 so the loop is stable; the bow
  // replenishes it). Low damping = a purer, more sustained string.
  loss_gain_ = std::clamp(0.99f - 0.09f * std::clamp(params.damping, 0.0f, 1.0f), 0.80f, 0.999f);

  // Tuning compensation: the two feedback registers (neck_out_/bridge_out_ are
  // consumed one sample after they are produced -> ~2 samples of loop delay not
  // in the lines) plus the bridge lowpass's exact phase delay at the
  // fundamental. Subtract them from the period before splitting.
  const float omega = kTwoPi / std::max(1.0f, base_period_);
  const float tau_lp = onepole_group_delay_samples(a, omega);
  comp_ = 2.0f + tau_lp;

  // Circular spans: each line is sized for the WHOLE period (plus bend-down
  // headroom ~+2 semitones and the interpolator stencil margin), not just its
  // note-on share, so a live bow-position sweep can move the split across the
  // full range without either line overflowing.
  const float eff = std::max(2.0f, base_period_ - comp_);
  const int full_span = static_cast<int>(eff * 1.3f) + 8;
  neck_size_ = std::min(capacity_, std::max(16, full_span));
  bridge_size_ = std::min(capacity_, std::max(16, full_span));
  neck_write_ = 0;
  bridge_write_ = 0;
  if (neck_ != nullptr) {
    for (int i = 0; i < neck_size_; ++i) neck_[static_cast<size_t>(i)] = 0.0f;
  }
  if (bridge_ != nullptr) {
    for (int i = 0; i < bridge_size_; ++i) bridge_[static_cast<size_t>(i)] = 0.0f;
  }

  // Bow velocity contour.
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);

  rosin_level_ = std::clamp(params.rosin, 0.0f, 1.0f) * kRosinDepth;

  // Elasto-plastic friction (off by default -> render() keeps the static-table
  // branch bit-identical). Derive the bristle constants from the same force /
  // damping / stribeck knobs, all in the model's normalised velocity units.
  elasto_plastic_ = params.elasto_plastic;
  bristle_z_ = 0.0f;
  if (elasto_plastic_) {
    const float stribeck01 = std::clamp(params.stribeck, 0.0f, 1.0f);
    ep_stribeck_v_ = kEpStribeckBase + kEpStribeckSpan * stribeck01;
    ep_z_max_ = kEpZMax;
    ep_z_ba_ = kEpBreakawayFrac * kEpZMax;
    ep_load_rate_ = ramp_coeff(kEpLoadTimeMs, sr);
  }

  // Sympathetic open-string bank (off by default -> render skips it entirely,
  // bit-identical). One-way driven by the bridge output, unity-peak normalized.
  static_assert(sizeof(kSympatheticNotes) / sizeof(kSympatheticNotes[0]) == kSympatheticModes_,
                "sympathetic note table must match the bank size");
  const float sympathetic = std::clamp(params.sympathetic, 0.0f, 1.0f);
  sympathetic_mix_ = sympathetic > 0.0f ? kSympatheticOutGain * sympathetic : 0.0f;
  if (sympathetic_mix_ > 0.0f) {
    const float srf = static_cast<float>(sr);
    const float r = std::exp(-6.907755279f / (srf * kSympatheticRingS));
    for (int i = 0; i < kSympatheticModes_; ++i) {
      SympatheticMode& m = sympathetic_[static_cast<size_t>(i)];
      const float freq = note_to_hz(static_cast<uint8_t>(kSympatheticNotes[i]));
      m.y1 = 0.0f;
      m.y2 = 0.0f;
      if (freq >= 0.45f * srf) {
        m.a1 = 0.0f;
        m.a2 = 0.0f;
        m.gain = 0.0f;
        continue;
      }
      const float w = kTwoPi * freq / srf;
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      m.gain = 1.0f - r;  // unity-peak (cancels the high-Q resonant boost)
    }
  }

  // Second (horizontal) polarization (off by default -> render skips it,
  // bit-identical). A detuned string loop sharing the bow; more damped than the
  // primary and weakly cross-coupled so the added feedback stays bounded.
  const float polarization = std::clamp(params.polarization, 0.0f, 1.0f);
  pol_couple_ = polarization > 0.0f ? kPolCoupleMax * polarization : 0.0f;
  pol_out_ = 0.0f;
  pol_lp_state_ = 0.0f;
  pol_write_ = 0;
  if (pol_couple_ > 0.0f) {
    pol_period_ = base_period_ * std::exp2(kPolDetuneCents / 1200.0f);
    pol_lp_alpha_ = 1.0f - kPolLpPole;
    pol_loss_ = kPolLoss;
    pol_drive_ = kPolDrive;
    pol_size_ = neck_size_;  // sized for a full detuned period (same span budget)
    if (pol_ != nullptr) {
      for (int i = 0; i < pol_size_; ++i) pol_[static_cast<size_t>(i)] = 0.0f;
    }
  }
}

float BowedStringVoiceCore::render(float pitch_ratio) noexcept {
  if (neck_ == nullptr || bridge_ == nullptr || neck_size_ < 8 || bridge_size_ < 8) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Live control: ramp the bow speed / force / position toward their CC targets
  // (control-rate host updates, audio-rate smoothing -> no zipper).
  max_bow_velocity_ += ctrl_coeff_ * (bow_speed_target_ - max_bow_velocity_);
  bow_slope_ += ctrl_coeff_ * (slope_target_ - bow_slope_);
  beta_ += ctrl_coeff_ * (beta_target_ - beta_);

  // Bow velocity: ramp the bow speed toward the target (1 while bowing, 0 once
  // the bow lifts), then scale to the model's velocity units.
  const float target = releasing_ ? 0.0f : 1.0f;
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  bow_level_ += coeff * (target - bow_level_);
  float bow_v = max_bow_velocity_ * bow_level_;
  if (rosin_level_ > 0.0f) {
    bow_v += max_bow_velocity_ * rosin_level_ * noise_.bipolar_at(drive_index_);
  }

  // Reflections from the previous delay-line outputs. Bridge: one-pole loss
  // filter then sign inversion; nut: ideal sign inversion.
  lp_state_ += lp_alpha_ * (bridge_out_ - lp_state_);
  const float bridge_refl = -loss_gain_ * lp_state_;
  const float nut_refl = -neck_out_;

  // String velocity at the bow point = sum of the incoming velocity waves, plus
  // (when gated on) the weakly-coupled horizontal polarization sharing the bow.
  const float string_primary = bridge_refl + nut_refl;
  float string_v = string_primary;
  if (pol_couple_ > 0.0f) string_v += pol_couple_ * pol_out_;
  const float dv = bow_v - string_v;

  // Bow friction: the velocity the bow injects equally into both delay lines.
  // Two models — the memoryless static table (default), or the gated
  // elasto-plastic bristle (Phase 4) that adds stick->slip hysteresis.
  float v_inj;
  if (elasto_plastic_) {
    // The bristle memory integrates the string's OWN relative velocity, excluding
    // the weak second-polarization coupling: feeding the detuned polarization beat
    // into the hysteresis state lets it re-trigger the slip a second time per
    // period in the low register (double-slip). The polarization still shares the
    // bow injection and radiates, so its acoustic "thickness" is unchanged.
    const float dv_ep = pol_couple_ > 0.0f ? bow_v - string_primary : dv;
    v_inj = elasto_plastic_injection(dv_ep);
  } else {
    // Bow table (memoryless friction curve): reflection/absorption coefficient in
    // [0,1]. exponent -4 == 1/x^4, so no pow() needed. The flat top (small dv) is
    // the sticking phase; the falling tails are the slipping phase. The injected
    // velocity dv*coeff scatters equally into both delay lines.
    const float s = bow_slope_ * dv + bow_offset_;
    const float base = std::fabs(s) + 0.75f;
    const float base2 = base * base;
    float bow_coeff = 1.0f / (base2 * base2);
    if (bow_coeff > 1.0f) bow_coeff = 1.0f;
    v_inj = dv * bow_coeff;
  }

  // Split the (compensated) period between the two lines and read/write them.
  const float eff = std::max(2.0f, base_period_ / ratio - comp_);
  const float neck_delay =
      std::clamp((1.0f - beta_) * eff, 1.0f, static_cast<float>(neck_size_ - 4));
  const float bridge_delay = std::clamp(beta_ * eff, 1.0f, static_cast<float>(bridge_size_ - 4));

  // Cross-couple at the junction: each line receives the OTHER side's reflection
  // plus the shared bow injection.
  neck_out_ =
      rt::lagrange3_fractional_delay(neck_, static_cast<size_t>(neck_size_), neck_write_,
                                     static_cast<int>(neck_delay * 256.0f), bridge_refl + v_inj);
  bridge_out_ =
      rt::lagrange3_fractional_delay(bridge_, static_cast<size_t>(bridge_size_), bridge_write_,
                                     static_cast<int>(bridge_delay * 256.0f), nut_refl + v_inj);
  ++drive_index_;
  // Output is the string velocity at the bridge (what drives the body).
  float dry = output_scale_ * bridge_out_;
  // Second (horizontal) polarization: its own lossy, detuned loop driven by the
  // shared bow injection (and coupled back through string_v above). Gated on.
  if (pol_couple_ > 0.0f) {
    pol_lp_state_ += pol_lp_alpha_ * (pol_out_ - pol_lp_state_);
    const float pol_refl = -pol_loss_ * pol_lp_state_;
    const float pol_delay =
        std::clamp(pol_period_ / ratio - comp_, 1.0f, static_cast<float>(pol_size_ - 4));
    pol_out_ = rt::lagrange3_fractional_delay(pol_, static_cast<size_t>(pol_size_), pol_write_,
                                              static_cast<int>(pol_delay * 256.0f),
                                              pol_refl + pol_drive_ * v_inj);
    dry += output_scale_ * kPolRadiation * pol_out_;
  }
  // Sympathetic open-string halo (gated; one-way, so it cannot destabilise the
  // waveguide loop). Driven by the sounding output level so the halo tracks the
  // played dynamics.
  if (sympathetic_mix_ > 0.0f) return dry + sympathetic_mix_ * sympathetic_process(dry);
  return dry;
}

float BowedStringVoiceCore::sympathetic_process(float x) noexcept {
  float sum = 0.0f;
  for (SympatheticMode& m : sympathetic_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * x;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  return sum;
}

float BowedStringVoiceCore::elasto_plastic_injection(float dv) noexcept {
  // Single-bristle elasto-plastic friction (Dupont/Avanzini-Serafin-Rocchesso):
  // the bristle deflection z is the friction memory that makes the stick->slip
  // transition hysteretic (the string re-grips along a different path than it
  // released along), warming the memoryless table's "dry" tone.
  const float v = dv;

  // Stribeck steady bristle: full grip near v=0, decaying to a small floor as the
  // contact slips. z_ss is signed with the relative velocity.
  const float ratio = v / ep_stribeck_v_;
  const float g = std::exp(-ratio * ratio);  // 1 at v=0, ->0 as |v| grows
  const float z_ss = (kEpZssFloor + (1.0f - kEpZssFloor) * g) * ep_z_max_;
  const float z_ss_signed = v >= 0.0f ? z_ss : -z_ss;

  // Adhesion (plastic) fraction alpha in [0,1]: 0 while the bristle is below
  // breakaway (pure elastic stick), rising smoothly to 1 as |z| approaches the
  // steady deflection (fully plastic slip). Zero whenever z and v disagree in
  // sign (the bristle is unloading) — that asymmetry IS the hysteresis.
  float alpha = 0.0f;
  const float az = std::fabs(bristle_z_);
  if ((v >= 0.0f) == (bristle_z_ >= 0.0f) && az > ep_z_ba_) {
    if (az < z_ss) {
      const float x = (az - ep_z_ba_) / std::max(z_ss - ep_z_ba_, 1.0e-6f);
      alpha = 0.5f * (1.0f - std::cos(kPi * x));  // smooth 0 -> 1
    } else {
      alpha = 1.0f;
    }
  }

  // Bristle evolution (forward Euler, load rate folds in dt): dz = rate*v*(1 -
  // alpha*z/z_ss). In stick alpha=0 so the bristle loads with the bow; in slip
  // alpha->1 and z saturates toward z_ss (the plastic ceiling).
  const float dz = ep_load_rate_ * v * (1.0f - alpha * bristle_z_ / z_ss_signed);
  bristle_z_ = std::clamp(bristle_z_ + dz, -ep_z_max_, ep_z_max_);

  // Feed the SHARP static bow table, but offset its operating point by the
  // bristle deflection: the table still switches abruptly between stick and slip
  // (so the Helmholtz sawtooth and its full harmonic series survive), yet the
  // breakaway and re-grip now happen at different relative velocities — the
  // elasto-plastic hysteresis loop that warms the "dry" memoryless slip.
  const float s = bow_slope_ * (dv - kEpHystOffset * bristle_z_) + bow_offset_;
  const float base = std::fabs(s) + 0.75f;
  const float base2 = base * base;
  float bow_coeff = 1.0f / (base2 * base2);
  if (bow_coeff > 1.0f) bow_coeff = 1.0f;
  return dv * bow_coeff;
}

void BowedStringVoiceCore::release() noexcept { releasing_ = true; }

void BowedStringVoiceCore::kill() noexcept {
  bow_level_ = 0.0f;
  lp_state_ = 0.0f;
  neck_out_ = 0.0f;
  bridge_out_ = 0.0f;
  bristle_z_ = 0.0f;
  pol_out_ = 0.0f;
  pol_lp_state_ = 0.0f;
  for (SympatheticMode& m : sympathetic_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  releasing_ = true;
}

}  // namespace sonare::midi::synth
