#include "midi/synth/flute_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kEpsilon;
using sonare::constants::kTwoPi;

// Mouth-pressure calibration. The exposed breath range lands the jet in its
// oscillating band — the knobs colour the tone, they do not gate it on and off.
// Dynamic LOUDNESS comes from the voice's velocity / amp VCA around this core,
// not from pushing the breath (below the band the jet never catches the edge and
// the note is silent). breath = base + span*level. The jet self-oscillates only
// where the cubic's slope at the operating point makes the loop gain exceed one:
// that is the high-pressure band (STK Flute blows at maxPressure ~1.1-1.3), not
// the low-pressure near-zero-slope region.
SONARE_TUNABLE(kBreathBase, 0.80f);
SONARE_TUNABLE(kBreathSpan, 0.35f);

// Jet delay / bore ratio band: jet_ratio colours the register the jet drives.
// The first register (the jet locking the fundamental) lives in ~[0.38, 0.62];
// smaller ratios overblow to the octave and above.
SONARE_TUNABLE(kJetRatioMin, 0.38f);
SONARE_TUNABLE(kJetRatioMax, 0.62f);

// Reflection coefficients (the two feedback taps). Clamped below the runaway
// region; the STK-stable operating point is ~0.5 each.
SONARE_TUNABLE(kReflectMax, 0.62f);

// The open-end reflection pole is `0.80 - 0.30*brightness`, voiced by ear at
// `string_loop.h`'s kLossVoicedSr and mapped onto the running rate there.

// Bore loss from damping: a mild reflection trim on top of the 0.5 reflections
// (which already keep the loop bounded). High damping quiets the resonance so an
// ocarina / blown bottle does not overblow.
SONARE_TUNABLE(kLossSpan, 0.18f);

// Pitch correction: the jet+bore lock lands a touch sharp of the naive full-
// period loop, so the loop delay is lengthened to bring the sounding note onto
// pitch. The earlier 1.0104 was set against a probe rather than a reference and
// left all eight flute captures flat by 7 to 12 cents; this centres them inside
// +-3, which is the spread between the voices and as close as one number gets.
SONARE_TUNABLE(kPitchCorrect, 1.0045f);

// Live-control smoothing time (ms).
SONARE_TUNABLE(kControlSmoothMs, 8.0f);

// Jet turbulence depth (a light seeded multiplicative jitter on the breath).
SONARE_TUNABLE(kBreathNoiseDepth, 0.10f);

// Onset chiff depth (the "speak" noise burst) and the level of the seeded burst
// pre-filled into the bore so the jet locks onto an f0 seed promptly.
SONARE_TUNABLE(kChiffDepth, 0.5f);
SONARE_TUNABLE(kBorePrefill, 0.05f);

// In-loop DC-blocker corner (~10 Hz): the jet's rectified DC does not radiate and
// would charge the bore, so the jet output is DC-blocked before it enters.
SONARE_TUNABLE(kDcCornerHz, 10.0f);

// Vibrato depth mapping: full depth is a gentle ~30 cents of pitch and a touch
// of level (a solo flute's own vibrato).
SONARE_TUNABLE(kVibPitchCents, 30.0f);
SONARE_TUNABLE(kVibAmp, 0.06f);

// Output trim: the driven jet loop settles with a raw bore peak that grows a
// little with pitch, so the output scale is frequency-compensated to keep a
// forte note near a flat target peak. peak_raw ~= kPeakBase + kPeakTilt*log2(f0/kPeakRefHz).
SONARE_TUNABLE(kOutputTargetPeak, 0.5f);
SONARE_TUNABLE(kPeakBase, 4.0f);
SONARE_TUNABLE(kPeakTilt, -0.65f);    // the driven peak falls with pitch (rich bass)
SONARE_TUNABLE(kPeakRefHz, 261.63f);  // middle C, the flute's home register

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

// Jet offset (asymmetry): a real jet is deflected to one side of the labium at
// rest, so it spends unequal time on each side and the drive is asymmetric — the
// source of the EVEN harmonics a concert flute has (the octave) that a symmetric
// odd cubic cannot produce. Modelled as an even (quadratic) term folded into the
// jet transfer; its DC is removed downstream by the DC blocker, leaving the 2nd
// (and higher even) harmonics.
SONARE_TUNABLE(kJetAsym, 0.5f);

// Even-harmonic pump gain: how hard the squared bore feedback (its 2f0 content)
// is injected back into the bore to voice the octave-dominant open-flue-pipe
// timbre. Calibrated so the octave sits below the fundamental but well above the
// odd partials (a bright concert-flute spectrum), staying bounded.
SONARE_TUNABLE(kEvenPumpGain, 0.6f);
// Even-pump DC follower corner (Hz): a slow lowpass that estimates the squared
// signal's DC so subtracting it leaves the 2f0 (and higher even) content.
SONARE_TUNABLE(kEvenPumpDcHz, 30.0f);

/// The jet function: the S-shaped saturating transfer of the air jet deflecting
/// across the labium (Fabre-Hirschberg lumped model / STK JetTable), with an
/// offset asymmetry (kJetAsym) that seeds the even harmonics. The odd cubic's
/// small-signal slope is inverting near zero (the oscillator drive); the clamp
/// bounds the limit cycle.
float jet_table(float x) noexcept {
  const float y = x * (x * x - 1.0f) + kJetAsym * x * x;
  return y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
}

}  // namespace

void FluteVoiceCore::start(const FlutePatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float srf = static_cast<float>(sr);
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;
  releasing_ = false;
  breath_level_ = 0.0f;
  lp_state_ = 0.0f;
  bore_out_ = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  jet_write_ = 0;
  vib_phase_ = 0.0f;

  const float f0 = note_to_hz(note);
  // Held for the bell-loop loss-law solve, which is re-run wherever the pole is
  // refreshed today (refresh_excitation_targets(), called both here and on
  // every live CC74 write) rather than only once at note-on.
  f0_ = f0;
  srf_ = srf;
  // The flute is open at both ends: the bore is a POSITIVE-feedback comb of one
  // full period, so the resonances land on the FULL harmonic series (f0, 2f0,
  // 3f0 …) the way an open flue pipe does. The jet buzzes the fundamental and
  // drives every harmonic; the asymmetric jet drive fills in the even harmonics
  // a stopped (odd-only) pipe would lack.
  const float period = kFluteBoreLengthPeriods * srf / std::max(1.0f, f0);
  bore_period_ = period * kPitchCorrect;
  jet_ratio_ = std::clamp(params.jet_ratio, kJetRatioMin, kJetRatioMax);

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);
  // A fresh note starts unmodulated; the matrix re-sets the offsets on its
  // first render, and a voice with no excitation route never touches them.
  breath01_base_ = level;
  force_mod01_ = 0.0f;
  bright_mod01_ = 0.0f;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);

  jet_reflection_ = std::min(std::clamp(params.jet_reflection, 0.0f, 1.0f), kReflectMax);
  end_reflection_ = std::min(std::clamp(params.end_reflection, 0.0f, 1.0f), kReflectMax);

  // Open-end reflection: damping alone sets the per-traversal gain, a flat
  // scalar that is already note- and rate-invariant because the loop is
  // traversed f0 times a second whatever the rate. damping is not live-
  // controllable, unlike brightness, so it is computed once here.
  damping_gain_ = std::clamp(1.0f - kLossSpan * std::clamp(params.damping, 0.0f, 1.0f), 0.5f, 1.0f);

  // Open-end reflection lowpass: brightness -> pole a (y += (1-a)(x - y)). A
  // brighter end reflects more upper partials (a smaller pole).
  bright01_base_ = std::clamp(params.brightness, 0.0f, 1.0f);
  // Both axes are derived here and nowhere else. A note-on copy of the mapping
  // is free to drift from the control-rate one, and a single rounding apart is
  // enough for the first CC to move a sound the host did not ask to move.
  refresh_excitation_targets();
  snap_excitation();

  // In-loop DC blocker pole (on the jet output).
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * kDcCornerHz / sr);

  // Even-harmonic pump (the octave-dominant open-pipe colour).
  even_gain_ = kEvenPumpGain;
  even_state_ = 0.0f;
  even_hp_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * kEvenPumpDcHz / static_cast<float>(sr)), 0.0f, 1.0f);

  // Tuning compensation: one feedback register (bore_out_ is consumed one sample
  // after it is produced) plus the reflection lowpass's phase delay at f0.
  const float omega = kTwoPi * f0 / srf;
  const float tau_lp = onepole_group_delay_samples(1.0f - lp_alpha_, omega);
  comp_ = 1.0f + tau_lp;
  // The jet's compensation is taken at the voiced rate: a sample count's
  // duration halves as the rate doubles, and the jet delay is a duration.
  const float voiced_srf = static_cast<float>(kLossVoicedSr);
  const float tau_voiced =
      onepole_group_delay_samples(1.0f - lp_alpha_voiced_, kTwoPi * f0 / voiced_srf);
  jet_comp_ = (1.0f + tau_voiced) * (srf / voiced_srf);

  // Both lines span the whole slab, because the line length is what bounds a
  // downward bend and the clamp enforcing it saturates silently -- a glide
  // simply stops descending while the note keeps sounding. What the note's own
  // period still decides is how much of the bore the onset seeds; the jet line
  // starts silent and so needs no span of its own.
  const float eff = std::max(2.0f, bore_period_ - comp_);
  prefill_span_ = std::min(capacity_, std::max(16, static_cast<int>(eff * 1.15f) + 8));
  // The seed IS the bore's history, so the write position starts just past it
  // and the first traversal reads back over the seeded span exactly as it did
  // when the line was no longer than that span.
  bore_write_ = capacity_ > 0 ? static_cast<size_t>(prefill_span_ % capacity_) : 0;
  jet_write_ = 0;

  // Contour + textures. Every seeded level is a per-sample draw voiced at
  // kLossVoicedSr, so each carries the noise law's gain.
  const float noise_gain = noise_gain_at_rate(sr);
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);
  breath_noise_ = std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth * noise_gain;
  chiff_level_ = std::clamp(params.chiff, 0.0f, 1.0f) * kChiffDepth * noise_gain;
  chiff_coeff_ = ramp_coeff(params.chiff_ms, sr);

  // Voice-local vibrato LFO.
  vib_depth_ = std::clamp(params.vibrato_depth, 0.0f, 1.0f);
  vib_depth_target_ = vib_depth_;
  const float vib_rate = std::clamp(params.vibrato_rate_hz, 0.1f, 12.0f);
  vib_inc_ = kTwoPi * vib_rate / srf;

  const float peak_est =
      std::clamp(kPeakBase + kPeakTilt * std::log2(std::max(1.0f, f0) / kPeakRefHz), 0.8f, 5.0f);
  output_scale_ = kOutputTargetPeak / peak_est;

  // Prompt speech: pre-fill the bore with a low-level seeded noise burst so the
  // jet has an f0 component to lock onto rather than swelling up from silence.
  // The jet span starts silent.
  const float prefill = kBorePrefill * breath_target_ * noise_gain;
  if (bore_ != nullptr) {
    // Past the seed the line has to be cleared rather than left alone: it is a
    // slab slot the previous note wrote, and everything outside the seed is
    // read before it is written.
    for (int i = prefill_span_; i < capacity_; ++i) bore_[static_cast<size_t>(i)] = 0.0f;
    for (int i = 0; i < prefill_span_; ++i) {
      bore_[static_cast<size_t>(i)] = prefill * noise_.bipolar_at(static_cast<uint64_t>(i));
    }
  }
  if (jet_ != nullptr) {
    for (int i = 0; i < capacity_; ++i) jet_[static_cast<size_t>(i)] = 0.0f;
  }
  drive_index_ = static_cast<uint64_t>(prefill_span_);

  // --- off-by-default advanced physics (Phase 4). When off, render() takes the
  // linear jet branch untouched (bit-identical). ---
  overblow_ = std::clamp(params.overblow, 0.0f, 1.0f);
  jet_turb_ = std::clamp(params.jet_turbulence, 0.0f, 1.0f);
  jet_turb_state_ = 0.0f;
  // Both followers are one-poles, so their coefficients are counts of samples
  // and move their corners with the rate exactly as the bore's pole did. Voiced
  // at 0.7 and 0.999 respectively, mapped here so the corner stays in Hz.
  jet_turb_alpha_ = 1.0f - loss_pole_at_rate(0.7f, sr);
  edge_hyst_ = std::clamp(params.edge_hysteresis, 0.0f, 1.0f);
  edge_hyst_state_ = 0.0f;
  edge_hyst_alpha_ = 1.0f - loss_pole_at_rate(0.999f, sr);
  vortex_ = std::clamp(params.vortex, 0.0f, 1.0f) * noise_gain;
}

float FluteVoiceCore::render(float pitch_ratio) noexcept {
  if (bore_ == nullptr || jet_ == nullptr || capacity_ < 8) return 0.0f;
  float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Live control: ramp the steady breath / brightness / vibrato depth toward
  // their CC targets (control-rate host updates, audio-rate smoothing -> no
  // zipper).
  breath_target_ += ctrl_coeff_ * (breath_ctrl_target_ - breath_target_);
  lp_alpha_ += ctrl_coeff_ * (lp_alpha_target_ - lp_alpha_);
  loss_gain_ += ctrl_coeff_ * (loss_gain_target_ - loss_gain_);
  vib_depth_ += ctrl_coeff_ * (vib_depth_target_ - vib_depth_);

  // Mouth-pressure contour: ramp toward the target (1 while blowing, 0 once
  // released), then the steady breath plus its turbulence and the onset chiff.
  const float target = releasing_ ? 0.0f : 1.0f;
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  breath_level_ += coeff * (target - breath_level_);
  float breath = breath_target_ * breath_level_;

  // Voice-local vibrato: a slow pitch (and slight level) undulation. Skipped
  // entirely at zero depth so a vibrato-free note is bit-identical.
  float vib_gain = 1.0f;
  if (vib_depth_ > 1.0e-4f) {
    const float v = std::sin(vib_phase_);
    vib_phase_ += vib_inc_;
    if (vib_phase_ >= kTwoPi) vib_phase_ -= kTwoPi;
    ratio *= std::exp2(kVibPitchCents * vib_depth_ * v * (1.0f / 1200.0f));
    vib_gain = 1.0f + kVibAmp * vib_depth_ * v;
  }

  if (breath_noise_ > 0.0f) {
    float n = noise_.bipolar_at(drive_index_);
    // 4b: amplitude-dependent, one-pole-shaped jet turbulence (gated).
    if (jet_turb_ > 0.0f) {
      jet_turb_state_ += jet_turb_alpha_ * (n - jet_turb_state_);
      n = (1.0f - jet_turb_) * n +
          jet_turb_ * (jet_turb_state_ + n) * (0.5f + 0.5f * breath_level_);
    }
    breath += breath * breath_noise_ * n;
  }
  if (chiff_level_ > 1.0e-4f) {
    breath += chiff_level_ * breath_target_ * noise_.bipolar_at(drive_index_ + 1u);
    chiff_level_ -= chiff_coeff_ * chiff_level_;
  }

  // Effective jet gain. 4a overblow (gated): push the jet gain toward the top of
  // the breath band so the upper register wins. 4c edge hysteresis (gated): the
  // register bias follows the breath direction. 4d vortex (gated): a rougher,
  // amplitude-gated source noise. All skipped when off (bit-identical).
  float jet_ref = jet_reflection_;
  if (overblow_ > 0.0f) {
    jet_ref *= 1.0f + 0.5f * overblow_ * std::max(0.0f, breath_level_ - 0.5f);
  }
  if (edge_hyst_ > 0.0f) {
    edge_hyst_state_ += edge_hyst_alpha_ * (breath_level_ - edge_hyst_state_);
    jet_ref *= 1.0f + 0.2f * edge_hyst_ * (edge_hyst_state_ - 0.5f);
  }

  // Open-pipe reflection from the previous bore output: one-pole loss lowpass,
  // POSITIVE feedback (an open-open pipe is a positive-feedback comb of the full
  // period — the full harmonic series). The returning pressure both drives the
  // jet (below) and re-enters the bore directly.
  lp_state_ += lp_alpha_ * (bore_out_ - lp_state_);
  const float temp = loss_gain_ * lp_state_;

  // Jet drive: the pressure difference across the flue drives the jet, which
  // convects (jet delay) and deflects across the labium (the cubic jet table).
  float pd = breath - jet_ref * temp;
  if (vortex_ > 0.0f) {
    pd += vortex_ * 0.3f * breath_level_ * breath_level_ * noise_.bipolar_at(drive_index_ + 2u);
  }
  const float bore_delay =
      std::clamp(bore_period_ / ratio - comp_, 1.0f, static_cast<float>(capacity_ - 4));
  // The jet rides the line as it was voiced, not the line at the running rate.
  const float jet_line =
      std::clamp(bore_period_ / ratio - jet_comp_, 1.0f, static_cast<float>(capacity_ - 4));
  const float jet_delay =
      std::clamp(jet_ratio_ * jet_line, 1.0f, static_cast<float>(capacity_ - 4));
  const float pd_j = rt::lagrange3_fractional_delay(
      jet_, static_cast<size_t>(capacity_), jet_write_, static_cast<int>(jet_delay * 256.0f), pd);
  const float jet_out = jet_table(pd_j);

  // DC-block the jet output, then drive the bore: jet flow plus the bore end
  // reflection.
  const float jet_dc = jet_out - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = jet_out;
  dc_y1_ = jet_dc;
  float into_bore = jet_dc + end_reflection_ * temp;

  // Even-harmonic pump: a half-wave rectified bore feedback carries a strong 2f0
  // component (the asymmetric jet drive of an offset flue), yet stays bounded by
  // the signal itself (|rect| <= |temp|), unlike a squared term which would
  // runaway. Strip its DC (a slow follower) and inject the 2f0 content into the
  // bore, whose octave resonance amplifies it — the open-flue-pipe octave that
  // voices above the odd partials.
  const float rect = temp > 0.0f ? temp : 0.0f;
  even_state_ += even_hp_alpha_ * (rect - even_state_);
  // Bound the injected pump: the jet path self-limits through its cubic clamp and
  // the direct end reflection is contractive (< 1), so bounding the pump too
  // keeps the whole loop BIBO-stable even at maximum drive (bright, undamped, high
  // reflection, high breath) — the pump is a colour, not an energy source.
  float pump = even_gain_ * (rect - even_state_);
  pump = pump < -1.5f ? -1.5f : (pump > 1.5f ? 1.5f : pump);
  into_bore += pump;
  bore_out_ = rt::lagrange3_fractional_delay(bore_, static_cast<size_t>(capacity_), bore_write_,
                                             static_cast<int>(bore_delay * 256.0f), into_bore);
  ++drive_index_;

  return output_scale_ * vib_gain * bore_out_;
}

void FluteVoiceCore::set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept {
  if ((present & kAxisForce) != 0u) {
    breath01_base_ = std::clamp(base.force, 0.0f, 1.0f);
  }
  if ((present & kAxisBrightness) != 0u) {
    bright01_base_ = std::clamp(base.brightness, 0.0f, 1.0f);
  }
  refresh_excitation_targets();
}

void FluteVoiceCore::set_excitation_mod(const ExcitationAxes& offsets) noexcept {
  force_mod01_ = offsets.force;
  bright_mod01_ = offsets.brightness;
  refresh_excitation_targets();
}

void FluteVoiceCore::refresh_excitation_targets() noexcept {
  const float b = std::clamp(breath01_base_ + force_mod01_, 0.0f, 1.0f);
  breath_ctrl_target_ = kBreathBase + kBreathSpan * b;

  // brightness is CC74-live, so the whole solve is redone on every write rather
  // than at note-on alone; freezing it in start() would silently kill the live
  // control.
  const float br = std::clamp(bright01_base_ + bright_mod01_, 0.0f, 1.0f);
  // The pole the ear voiced, at kLossVoicedSr. The clamp is provably slack for
  // any br in [0,1] -- it ranges [0.50, 0.80], well inside [0, 0.95] -- and is
  // kept only to reproduce the shipped formula exactly.
  const float a_voiced = std::clamp(0.80f - 0.30f * br, 0.0f, 0.95f);
  lp_alpha_target_ = 1.0f - loss_pole_at_rate(a_voiced, srf_);
  lp_alpha_voiced_ = 1.0f - loss_pole_at_rate(a_voiced, kLossVoicedSr);
  // The per-traversal gain needs no mapping: the loop is traversed f0 times a
  // second whatever the rate, so `damping_gain_` already means the same thing.
  loss_gain_target_ = damping_gain_;
}

void FluteVoiceCore::set_vibrato(float depth01) noexcept {
  vib_depth_target_ = depth01 < 0.0f ? 0.0f : (depth01 > 1.0f ? 1.0f : depth01);
}

void FluteVoiceCore::snap_excitation() noexcept {
  breath_target_ = breath_ctrl_target_;
  lp_alpha_ = lp_alpha_target_;
  loss_gain_ = loss_gain_target_;
  vib_depth_ = vib_depth_target_;
}

void FluteVoiceCore::release() noexcept { releasing_ = true; }

void FluteVoiceCore::kill() noexcept {
  breath_level_ = 0.0f;
  lp_state_ = 0.0f;
  bore_out_ = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  chiff_level_ = 0.0f;
  jet_turb_state_ = 0.0f;
  releasing_ = true;
}

}  // namespace sonare::midi::synth
