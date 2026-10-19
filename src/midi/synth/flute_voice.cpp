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

using sonare::constants::kCentsPerOctave;
using sonare::constants::kEpsilon;
using sonare::constants::kPi;
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

// Jet-drive source gain G: the bore pressure the flow split's time difference
// injects at the fundamental, per unit jet speed at full breath.
SONARE_TUNABLE(kJetDriveGain, 3.0f);
// Leak corner (Hz) of the jet displacement's integral of the acoustic flow.
SONARE_TUNABLE(kJetDisplacementLeakHz, 10.0f);
// Share of the jet delay's sqrt(B_ref/B) breath law the player cancels by shortening the jet
// with the breath (the lips close as the flutist blows harder); 0 = none, 1 = a fixed delay.
SONARE_TUNABLE(kJetLengthCompensation, 0.0f);
// Floor on the reference breath the jet delay is normalised to: one velocity step.
constexpr float kJetBreathFloor = 1.0f / 127.0f;
// Strouhal corner f*tau of the jet's receptivity: the deflection follows the returning flow up to
// this frequency over the jet delay, so the odd harmonics the half-period delay would regenerate
// are not. 0 = none. At 1 the third stays under the second from C4 to C6 (y0 1, K 0.03, G 6).
SONARE_TUNABLE(kJetReceptivityStrouhal, 0.0f);
// Low Strouhal corner of the same receptivity, replacing the fixed displacement leak when set: a
// jet does not amplify a near-steady deflection, and a leak at 10 Hz let the bore's sub-audio
// return swing the jet's mean offset (±8 Hz sidebands at -28 dB on E4). 0 = the fixed leak.
SONARE_TUNABLE(kJetReceptivityStrouhalLow, 0.0f);
// Held level per octave above kPeakRefHz on the jet-drive path (dB): the bore level the source
// settles at falls 2.6 dB/oct where the reference rises 3; a monopole's radiation is +6. 0 = flat.
SONARE_TUNABLE(kJetDriveLevelTiltDb, 0.0f);

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
                           Velocity16 velocity, uint64_t seed) noexcept {
  const double host_sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  // The loop runs at the smallest multiple of the host rate whose bore and jet lines clear their
  // floor; everything below is built for that internal rate.
  const int factor = settle_loop_oversample(
      [&](int f) { return configure(params, host_sr * f, note, velocity, seed); });
  decimator_.configure(factor);
}

LoopBudget FluteVoiceCore::configure(const FlutePatchParams& params, double sr, uint8_t note,
                                     Velocity16 velocity, uint64_t seed) noexcept {
  const float srf = static_cast<float>(sr);
  noise_ = VoiceRandomSequence(seed);
  killed_ = false;
  drive_index_ = 0;
  breath_.releasing = false;
  breath_.level = 0.0f;
  breath_.overshoot = 0.0f;
  lp_state_ = 0.0f;
  bore_.out = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  jet_write_ = 0;
  vib_phase_ = 0.0f;

  const float f0 = note_to_hz(note);
  // Held for retune_loop_comp(), which follows the pole wherever it moves.
  f0_ = f0;
  srf_ = srf;
  // The flute is open at both ends: the bore is a POSITIVE-feedback comb of one
  // full period, so the resonances land on the FULL harmonic series (f0, 2f0,
  // 3f0 …) the way an open flue pipe does. The jet buzzes the fundamental and
  // drives every harmonic; the asymmetric jet drive fills in the even harmonics
  // a stopped (odd-only) pipe would lack.
  // The jet locks the loop on its own period once the loop's phase is compensated: at 48 kHz
  // the fundamental reads within a cent of equal temperament over notes 56..80 with no trim.
  bore_.period = kFluteBoreLengthPeriods * srf / std::max(1.0f, f0);
  jet_ratio_ = std::clamp(params.jet_ratio, kJetRatioMin, kJetRatioMax);

  const float vel01 = velocity.f7() / 127.0f;
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);
  // A fresh note starts unmodulated; the matrix re-sets the offsets on its
  // first render, and a voice with no excitation route never touches them.
  excite_.force01_base = level;
  excite_.force_mod01 = 0.0f;
  excite_.bright_mod01 = 0.0f;
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
  excite_.bright01_base = std::clamp(params.brightness, 0.0f, 1.0f);
  // Both axes are derived here and nowhere else. A note-on copy of the mapping
  // is free to drift from the control-rate one, and a single rounding apart is
  // enough for the first CC to move a sound the host did not ask to move.
  refresh_excitation_targets();
  snap_excitation();
  excite_.remember_base();

  // In-loop DC blocker pole (on the jet output).
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * kDcCornerHz / sr);

  // Even-harmonic pump (the octave-dominant open-pipe colour).
  even_gain_ = kEvenPumpGain;
  even_state_ = 0.0f;
  even_hp_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * kEvenPumpDcHz / static_cast<float>(sr)), 0.0f, 1.0f);

  // Jet-drive source. The breath B is blowing pressure, so Uj = sqrt(2P/rho) goes as sqrt(B) and
  // the jet delay W/(kappa Uj), taken against the patch's own breath, is jet_ratio period
  // sqrt(B_ref/B).
  jet_drive_ = params.jet_drive;
  jet_offset_ = std::clamp(params.jet_offset, 0.0f, 2.0f);
  jet_speed_ref_ =
      std::sqrt(std::max(std::clamp(params.breath_pressure, 0.0f, 1.0f), kJetBreathFloor));
  jet_drive_norm_ = 1.0f / std::max(2.0f * std::sin(kPi * f0 / srf), 1.0e-6f);
  jet_flow_z1_ = std::tanh(-jet_offset_);
  jet_eta_ = 0.0f;
  jet_eta_leak_ = std::exp(-kTwoPi * kJetDisplacementLeakHz / srf);
  // Receptivity band: both corners sit at a Strouhal number over the jet delay at the patch's own
  // breath, and each pole's phase delay at f0 (for the leak, its shortfall against a pure
  // integrator's) comes off the jet line so the delay alone still sets the tuning.
  const float omega0 = kTwoPi * f0 / srf;
  jet_lp_state_ = 0.0f;
  jet_lp_alpha_ = 0.0f;
  jet_lp_comp_ = 0.0f;
  if (jet_drive_ && kJetReceptivityStrouhalLow > 0.0f) {
    jet_eta_leak_ = std::exp(-kTwoPi * kJetReceptivityStrouhalLow / (jet_ratio_ * bore_.period));
    jet_lp_comp_ -=
        (kPi - omega0) / (2.0f * omega0) - onepole_group_delay_samples(jet_eta_leak_, omega0);
  }
  if (jet_drive_ && kJetReceptivityStrouhal > 0.0f) {
    jet_lp_alpha_ =
        1.0f - std::exp(-kTwoPi * kJetReceptivityStrouhal / (jet_ratio_ * bore_.period));
    jet_lp_comp_ += onepole_group_delay_samples(1.0f - jet_lp_alpha_, omega0);
  }

  retune_loop_comp();
  // The jet line is read at one sample or more as well, and it is the shorter of the two: its
  // delay is the jet ratio of the period, all of it a duration. On the jet-drive path the
  // shortest is at full breath.
  const float jet_span = jet_drive_ ? jet_ratio_ * jet_speed_ref_ : jet_ratio_;
  const LoopBudget budget =
      worst_loop_budget(loop_budget(bore_.period, bore_.comp, 1.0f, sr),
                        loop_budget(bore_.period, 0.0f, std::max(1.0f, 1.0f / jet_span), sr, 0.0f));

  // Both lines span the whole slab, because the line length is what bounds a
  // downward bend and the clamp enforcing it saturates silently -- a glide
  // simply stops descending while the note keeps sounding. What the note's own
  // period still decides is how much of the bore the onset seeds; the jet line
  // starts silent and so needs no span of its own. The seed IS the bore's
  // history, so the write position starts just past it and the first traversal
  // reads back over the seeded span exactly as it did when the line was no
  // longer than that span.
  bore_.configure(bore_.buffer, capacity_, bore_.period, bore_.comp, 1.15f);
  jet_write_ = 0;

  // Contour + textures. Every seeded level is a per-sample draw voiced at
  // kLossVoicedSr, so each carries the noise law's gain.
  const float noise_gain = noise_gain_at_rate(sr);
  breath_.attack_coeff = ramp_coeff(params.attack_ms, sr);
  breath_.release_coeff = ramp_coeff(params.release_ms, sr);
  breath_.set_overshoot_at_velocity(params.onset_breath_overshoot_db,
                                    params.onset_breath_overshoot_vel_db, vel01,
                                    params.onset_breath_settle_ms, sr);
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
  if (jet_drive_ && kJetDriveLevelTiltDb != 0.0f) {
    output_scale_ *= std::pow(10.0f, kJetDriveLevelTiltDb * std::log2(f0 / kPeakRefHz) / 20.0f);
  }

  // Prompt speech: pre-fill the bore with a low-level seeded noise burst so the
  // jet has an f0 component to lock onto rather than swelling up from silence.
  // The jet span starts silent. Past the seed the bore has to be cleared rather
  // than left alone: it is a slab slot the previous note wrote, and everything
  // outside the seed is read before it is written.
  const float prefill = kBorePrefill * breath_target_ * noise_gain;
  bore_.seed(prefill, noise_);
  if (jet_ != nullptr) {
    for (int i = 0; i < capacity_; ++i) jet_[static_cast<size_t>(i)] = 0.0f;
  }
  drive_index_ = static_cast<uint64_t>(bore_.prefill_span);

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
  vortex_loss_ = std::max(params.vortex_loss, 0.0f);
  return budget;
}

float FluteVoiceCore::render(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  return decimator_.run([&] { return render_internal(pitch_ratio); });
}

float FluteVoiceCore::render_internal(float pitch_ratio) noexcept {
  if (killed_) return 0.0f;
  if (bore_.buffer == nullptr || jet_ == nullptr || capacity_ < 8) return 0.0f;
  float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Live control: ramp the steady breath / brightness / vibrato depth toward
  // their CC targets (control-rate host updates, audio-rate smoothing -> no
  // zipper).
  breath_target_ += ctrl_coeff_ * (breath_ctrl_target_ - breath_target_);
  lp_alpha_ += ctrl_coeff_ * (lp_alpha_target_ - lp_alpha_);
  if (lp_alpha_ != comp_alpha_) retune_loop_comp();
  loss_gain_ += ctrl_coeff_ * (loss_gain_target_ - loss_gain_);
  vib_depth_ += ctrl_coeff_ * (vib_depth_target_ - vib_depth_);

  // Mouth-pressure contour: ramp toward the target (1 while blowing, 0 once
  // released), then the steady breath plus its turbulence and the onset chiff.
  breath_.advance();
  float breath = breath_target_ * breath_.level;
  const float steady_breath = breath;

  // Voice-local vibrato: a slow pitch (and slight level) undulation. Skipped
  // entirely at zero depth so a vibrato-free note is bit-identical.
  float vib_gain = 1.0f;
  if (vib_depth_ > 1.0e-4f) {
    const float v = std::sin(vib_phase_);
    vib_phase_ += vib_inc_;
    if (vib_phase_ >= kTwoPi) vib_phase_ -= kTwoPi;
    ratio *= std::exp2(kVibPitchCents * vib_depth_ * v * (1.0f / kCentsPerOctave));
    vib_gain = 1.0f + kVibAmp * vib_depth_ * v;
  }

  if (breath_noise_ > 0.0f) {
    float n = noise_.bipolar_at(drive_index_);
    // 4b: amplitude-dependent, one-pole-shaped jet turbulence (gated).
    if (jet_turb_ > 0.0f) {
      jet_turb_state_ += jet_turb_alpha_ * (n - jet_turb_state_);
      n = (1.0f - jet_turb_) * n +
          jet_turb_ * (jet_turb_state_ + n) * (0.5f + 0.5f * breath_.level);
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
    jet_ref *= 1.0f + 0.5f * overblow_ * std::max(0.0f, breath_.level - 0.5f);
  }
  if (edge_hyst_ > 0.0f) {
    edge_hyst_state_ += edge_hyst_alpha_ * (breath_.level - edge_hyst_state_);
    jet_ref *= 1.0f + 0.2f * edge_hyst_ * (edge_hyst_state_ - 0.5f);
  }

  // Open-pipe reflection from the previous bore output: one-pole loss lowpass,
  // POSITIVE feedback (an open-open pipe is a positive-feedback comb of the full
  // period — the full harmonic series). The returning pressure both drives the
  // jet (below) and re-enters the bore directly.
  lp_state_ += lp_alpha_ * (bore_.out - lp_state_);
  float temp = loss_gain_ * lp_state_;
  // Mouth vortex loss (gated): the flow u the returning pressure r drives solves u + K u|u| = r.
  if (vortex_loss_ > 0.0f) {
    temp = 2.0f * temp / (1.0f + std::sqrt(1.0f + 4.0f * vortex_loss_ * std::fabs(temp)));
  }

  // Jet drive: the pressure difference across the flue drives the jet, which
  // convects (jet delay) and deflects across the labium (the cubic jet table).
  // On the jet-drive path the line carries the deflection alone: the mouth
  // pressure's fluctuation and the returning bore pressure, not the mean breath.
  float pd = (jet_drive_ ? breath - steady_breath : breath) - jet_ref * temp;
  if (vortex_ > 0.0f) {
    pd += vortex_ * 0.3f * breath_.level * breath_.level * noise_.bipolar_at(drive_index_ + 2u);
  }
  // A fraction of the PERIOD, not of the line: the line is short by a fixed sample count.
  float jet_period = jet_ratio_ * bore_.period / ratio;
  // The jet speed over its full-breath value. The contour level carries the onset overshoot,
  // so the overshoot applies to the blowing pressure, as on the brass mouth.
  float jet_speed = 0.0f;
  if (jet_drive_) {
    jet_speed =
        std::sqrt(std::max(0.0f, (breath_target_ - kBreathBase) / kBreathSpan) * breath_.level);
    const float stretch = jet_speed_ref_ / std::max(jet_speed, 1.0e-6f);
    jet_period *= kJetLengthCompensation > 0.0f
                      ? std::pow(stretch, 1.0f - std::min(kJetLengthCompensation, 1.0f))
                      : stretch;
  }
  // The receptivity pole's phase delay at f0 is taken off the line so the delay still sets the
  // tuning.
  jet_period -= jet_lp_comp_;
  const float jet_delay = std::clamp(jet_period, 1.0f, static_cast<float>(capacity_ - 4));
  jet_delay_ = jet_delay;
  float pd_j = rt::lagrange3_fractional_delay(jet_, static_cast<size_t>(capacity_), jet_write_,
                                              static_cast<int>(jet_delay * 256.0f), pd);
  float jet_out;
  if (jet_drive_) {
    if (jet_lp_alpha_ > 0.0f) {
      jet_lp_state_ += jet_lp_alpha_ * (pd_j - jet_lp_state_);
      pd_j = jet_lp_state_;
    }
    // dQ/dt of the flow split Q ~ b Uj tanh((eta - y0)/b), eta in half-widths.
    // The jet deflection is the displacement the acoustic flow imposes: its integral, unit gain at
    // f0.
    jet_eta_ = jet_eta_leak_ * jet_eta_ + pd_j / jet_drive_norm_;
    const float flow = std::tanh(jet_eta_ - jet_offset_);
    jet_out = kJetDriveGain * jet_speed * jet_drive_norm_ * (flow - jet_flow_z1_);
    jet_flow_z1_ = flow;
  } else {
    jet_out = jet_table(pd_j);
  }

  // DC-block the jet output, then drive the bore: jet flow plus the bore end
  // reflection.
  const float jet_dc = jet_out - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = jet_out;
  dc_y1_ = jet_dc;
  const float gate = breath_.gate();
  float into_bore = WindBore::injection(end_reflection_ * temp, jet_dc, gate);

  // Even-harmonic pump: a half-wave rectified bore feedback carries a strong 2f0
  // component (the asymmetric jet drive of an offset flue), yet stays bounded by
  // the signal itself (|rect| <= |temp|), unlike a squared term which would
  // runaway. Strip its DC (a slow follower) and inject the 2f0 content into the
  // bore, whose octave resonance amplifies it — the open-flue-pipe octave that
  // voices above the odd partials.
  // The jet-drive path takes its even harmonics from the jet offset instead.
  if (!jet_drive_) {
    const float rect = temp > 0.0f ? temp : 0.0f;
    even_state_ += even_hp_alpha_ * (rect - even_state_);
    // Bound the injected pump: the jet path self-limits through its cubic clamp and
    // the direct end reflection is contractive (< 1), so bounding the pump too
    // keeps the whole loop BIBO-stable even at maximum drive (bright, undamped, high
    // reflection, high breath) — the pump is a colour, not an energy source.
    float pump = even_gain_ * (rect - even_state_);
    pump = pump < -1.5f ? -1.5f : (pump > 1.5f ? 1.5f : pump);
    into_bore = WindBore::injection(into_bore, pump, gate);
  }
  bore_.advance(into_bore, ratio);
  ++drive_index_;

  // The saturated jet holds its own amplitude, but the onset's extra mouth
  // pressure speeds the jet as sqrt(P), and the radiated flow follows the jet speed.
  // The jet-drive source already scales with the jet speed.
  const float out = output_scale_ * vib_gain * bore_.out;
  return breath_.overshoot > 0.0f && !jet_drive_ ? out * std::sqrt(breath_.overshoot_ratio()) : out;
}

void FluteVoiceCore::set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept {
  if ((present & kAxisForce) != 0u) hold_onset();
  excite_.set_base(base, present);
  refresh_excitation_targets();
}

void FluteVoiceCore::restore_excitation_base() noexcept {
  excite_.restore_base();
  refresh_excitation_targets();
}

void FluteVoiceCore::set_excitation_mod(const ExcitationAxes& offsets) noexcept {
  excite_.set_mod(offsets);
  refresh_excitation_targets();
}

void FluteVoiceCore::refresh_excitation_targets() noexcept {
  const float b = std::clamp(excite_.force01_base + excite_.force_mod01, 0.0f, 1.0f);
  breath_ctrl_target_ = kBreathBase + kBreathSpan * b;

  // brightness is CC74-live, so the whole solve is redone on every write rather
  // than at note-on alone; freezing it in start() would silently kill the live
  // control.
  const float br = std::clamp(excite_.bright01_base + excite_.bright_mod01, 0.0f, 1.0f);
  // The pole the ear voiced, at kLossVoicedSr. The clamp is provably slack for
  // any br in [0,1] -- it ranges [0.50, 0.80], well inside [0, 0.95] -- and is
  // kept only to reproduce the shipped formula exactly.
  const float a_voiced = std::clamp(0.80f - 0.30f * br, 0.0f, 0.95f);
  lp_alpha_target_ = 1.0f - loss_pole_at_rate(a_voiced, srf_);
  // The per-traversal gain needs no mapping: the loop is traversed f0 times a
  // second whatever the rate, so `damping_gain_` already means the same thing.
  loss_gain_target_ = damping_gain_;
}

void FluteVoiceCore::retune_loop_comp() noexcept {
  // One feedback register (bore_.out is consumed one sample after it is
  // produced) plus the reflection lowpass's phase delay at f0, less the jet
  // path's DC-blocker phase LEAD there, as the flue pipe takes it: left in, the
  // lead read +20 cents at note 36 and +11 at 48.
  const float a = 1.0f - lp_alpha_;
  const float omega = kTwoPi * f0_ / srf_;
  const float sw = std::sin(omega);
  const float cw = std::cos(omega);
  const float phase_dc = std::atan2(sw, 1.0f - cw) - std::atan2(dc_r_ * sw, 1.0f - dc_r_ * cw);
  bore_.comp = 1.0f + onepole_group_delay_samples(a, omega) - phase_dc / std::max(omega, 1.0e-6f);
  comp_alpha_ = lp_alpha_;
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

void FluteVoiceCore::release() noexcept { breath_.release(); }

void FluteVoiceCore::kill() noexcept {
  killed_ = true;
  decimator_.reset();
  breath_.level = 0.0f;
  lp_state_ = 0.0f;
  bore_.out = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  chiff_level_ = 0.0f;
  jet_turb_state_ = 0.0f;
  breath_.releasing = true;
}

}  // namespace sonare::midi::synth
