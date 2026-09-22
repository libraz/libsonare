#include "midi/synth/brass_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kPi;
using sonare::constants::kTwoPi;

// Mouth-pressure scale: the player's lung pressure enters the mouthpiece. The
// lips buzz only above a threshold loop gain, and mouth * lip_couple is the
// active gain, so the mouth pressure must reach ~unity for the lips to speak —
// this scale brings the calibrated breath band up to that buzzing pressure.
SONARE_TUNABLE(kMouthScale, 1.0f);

// Mouth-pressure calibration: the exposed breath range lands in the strong
// buzzing band ([~0.72, ~1.0]) — the knobs colour the tone, they do not gate it
// on and off. Dynamic LOUDNESS comes from the voice's velocity / amp VCA around
// this core, not from pushing the breath, so breath varies only mildly with the
// note. breath = base + span*level.
SONARE_TUNABLE(kBreathBase, 0.72f);
SONARE_TUNABLE(kBreathSpan, 0.28f);

// Lip valve calibration. The lip is a resonant reflection coefficient at the
// mouthpiece: coeff = clamp(offset - couple*lip_displacement, -1, 1), and the
// injection is the reed-style flow inj = mouth + dp*coeff (the same topology as
// the reed table, but the coefficient is the RESONANT buzzing lip rather than a
// memoryless table — this is where the harmonic drive lives, so the octave and
// upper partials survive into the bore instead of a bare sine). The OUTWARD-
// striking sign is NEGATIVE (rising lip displacement lowers the reflection in the
// phase that reinforces the note), which locks the buzz onto the fundamental
// rather than a mistuned mode between the comb teeth. The rest reflection is
// biased a touch negative for a brighter operating point.
SONARE_TUNABLE(kLipOffset, -0.1f);
SONARE_TUNABLE(kLipCouple, 4.5f);
// Mouthpiece coefficient of the one-sided valve, applied to the Bernoulli flow.
// Seeded at unity and kept there: the valve is quieter than the reflection
// coefficient it replaces, and the level belongs to the radiation makeup, not
// here — raising this instead drives the loop into period doubling.
SONARE_TUNABLE(kLipFlowScale, 1.0f);
// Amplitude-dependent propagation: the fraction the bore delay shortens at full
// bore_nonlinearity and full normalised bore pressure. Seeded from the physics
// rather than fitted -- beta = (gamma+1)/2 = 1.2 for air, times a fortissimo
// bore pressure of about 5% of atmospheric.
SONARE_TUNABLE(kBoreSpeedSpan, 0.06f);
// Lip resonator quality factor from lip_damping, held CONSTANT-Q (bandwidth
// proportional to the note) so the lip stays selective at low notes — a
// fixed-radius resonator is wider than the fundamental in the tuba range and
// lets the octave win, jumping the register. A tight lip is a sharp, high-Q
// resonance (a bright, brassy buzz); a loose lip is broader and lower-Q (a
// mellow tone). Q = min + span*(1 - lip_damping).
SONARE_TUNABLE(kLipQMin, 8.0f);
SONARE_TUNABLE(kLipQSpan, 22.0f);
// Lip tension detunes the lip resonance a little above / below the note (the
// embouchure centre). Small — a few percent.
SONARE_TUNABLE(kLipTuneSpan, 0.04f);  // f_lip = f0 * (1 + span*(tension - 0.5))
// How far the per-sample pitch factor may drift from the one the lip is tuned
// for before the resonator is redesigned — 1.04 cents, under what a bend needs
// to sound continuous and over a shallow vibrato's own excursion. Not a
// SONARE_TUNABLE: it trades CPU against pitch resolution, with no reference.
constexpr float kLipRetuneTolerance = 0.0006f;
// Pitch correction: an outward-striking lip oscillates just ABOVE its resonance
// (Fletcher 1979), so the played note lands a touch sharp of the bore/lip lock;
// the loop delay is lengthened to bring it back onto pitch. Co-calibrated with
// the DC-blocker phase compensation below so the sounding pitch stays within a
// few cents across the whole keyboard (probe-measured).
SONARE_TUNABLE(kPitchCorrect, 1.0063f);

// Bell reflection loss from damping: < 1 so the loop is stable (the breath
// replenishes it). Low damping = a purer, more sustained bore.
SONARE_TUNABLE(kLossBase, 0.995f);
SONARE_TUNABLE(kLossSpan, 0.08f);
SONARE_TUNABLE(kLossFloor, 0.85f);
SONARE_TUNABLE(kLossCeil, 0.999f);

// Bell loop-lowpass depth: brightness maps to the one-pole pole (a brighter bell
// reflects more upper partials). The conical bias darkens a conical brass.
SONARE_TUNABLE(kBellPoleSpan, 0.7f);
SONARE_TUNABLE(kConicalDarken, 0.12f);  // extra pole for conical (horn / tuba)
// One-flare bell: how far brightness and the bore shape move the corner, in
// octaves. Both seeded off the mapping they replace, read at its midpoint --
// its slope there is 2.75 octaves per unit brightness, and the conical bias is
// 0.48 octaves down.
SONARE_TUNABLE(kBellBrightOct, 2.75f);
SONARE_TUNABLE(kConicalOct, 0.48f);

// How much of the bell highpass's loss at the fundamental is given back, as an
// exponent: 1 restores it exactly, 0 leaves the radiated level as the filter made
// it. Only the note-to-note balance rides on it — every timbre metric is blind to
// a per-note gain — and the references pick 0.35, which holds the register
// profile at the 0.6 dB the bore pressure had while 1.0 spreads it to 2.3.
SONARE_TUNABLE(kBellRadiationNorm, 0.35f);
// Make-up for what radiating costs in loudness. The bore pressure is a fat
// near-sine with a 5 dB crest; the radiated wave is the spiky one a reference
// brass has, 12 to 17 dB, so holding the peak where it was leaves the family
// 8 dB under the rest of the bank. Flat, so it is loudness only.
SONARE_TUNABLE(kBellRadiationMakeup, 2.45f);

// Live-control smoothing time (ms).
SONARE_TUNABLE(kControlSmoothMs, 8.0f);

// Breath turbulence depth (a light seeded jitter on the mouth pressure).
SONARE_TUNABLE(kBreathNoiseDepth, 0.08f);

// Onset chiff depth (the tonguing "speak" noise burst) and the level of the
// seeded burst pre-filled into the bore so the note speaks promptly.
SONARE_TUNABLE(kChiffDepth, 0.5f);
SONARE_TUNABLE(kBorePrefill, 0.03f);

// In-loop DC blocker: the positive-feedback comb has a sub-fundamental (DC) mode
// the rectified lip drive can excite, so the injection is DC-blocked before it
// enters the bore. A fixed low corner leaves a frequency-dependent phase LEAD
// that sharpens the low range (a quarter-tone at the tuba pedal), so the corner
// tracks the pitch (floored) to keep the lead a bounded fraction of the period,
// and the residual lead is folded into the loop compensation — the same
// pitch-tracked highpass the reed cone (a full-period positive-feedback comb of
// the same topology) uses.
SONARE_TUNABLE(kDcCornerFracF0, 0.06f);
SONARE_TUNABLE(kDcCornerFloorHz, 10.0f);
// Fraction of the analytic highpass phase lead folded into comp: the nonlinear
// lip oscillation sits between the linear loop resonance and the free lip, so
// only part of the lead detunes the sounding pitch (probe-calibrated).
SONARE_TUNABLE(kDcCompScale, 0.5f);

// Output trim: the driven loop settles with a raw bore peak that grows with the
// note (~2.3 at the bottom of the range to ~7 at the top), so the output scale is
// frequency-compensated to keep a forte note near a flat target peak across the
// keyboard. peak_raw ~= kPeakBase + kPeakTilt*log2(f0/kPeakRefHz).
SONARE_TUNABLE(kOutputTargetPeak, 0.6f);
SONARE_TUNABLE(kPeakBase, 2.33f);
SONARE_TUNABLE(kPeakTilt, 0.93f);
SONARE_TUNABLE(kPeakRefHz, 44.0f);

// Cuivré dynamics (only when params.cuivre_dynamics > 0): the shock steepening
// tracks the played dynamic. The mouth pressure is normalised over the buzzing
// band and SQUARED — a shock forms superlinearly with blowing pressure, so a
// soft note stays round and the brassy bloom concentrates near ff. The gain lets
// a hard note push the effective brassiness above its nominal value (a real ff
// brass blares well past its mezzo colour).
SONARE_TUNABLE(kCuivreDynGain, 1.8f);

// --- 4a cuivré (only when params.brassiness > 0) ---
// Steepening drive and asymmetry: how hard the normalised bore output is pushed
// through the shock shaper, and how asymmetric the shock front is (the |x| term
// adds even harmonics so the spectrum is a full shock, not an odd-only clip).
// tanh normalisation keeps the peak while the curvature blooms the harmonics.
SONARE_TUNABLE(kCuivreDrive, 9.0f);
SONARE_TUNABLE(kCuivreAsym, 0.5f);
// Low-register drive compensation: the linear bore grows more sinusoidal toward
// low f0 (its positive-feedback comb carries fewer partials there), so at a fixed
// drive the shock shaper barely saturates and the brass formant never blooms in
// the low register. The drive is boosted below the reference by (ref/f0)^2 (the
// ratio capped so the boost saturates), leaving the calibrated mid/high brass
// untouched (the factor is 1 at and above the reference).
SONARE_TUNABLE(kCuivreDriveRefHz, 175.0f);
SONARE_TUNABLE(kCuivreDriveRatioMax, 2.3f);
// Max wet mix of the shaped (brassy) signal at full brassiness.
SONARE_TUNABLE(kCuivreMixMax, 0.85f);

// --- 4b mute (only when params.mute > 0) ---
// Muted upper formant (Hz) and its resonance: the nasal honk of a straight/cup
// mute. The formant peak is boosted and the direct low-mid scooped.
SONARE_TUNABLE(kMuteFormantHz, 1800.0f);
SONARE_TUNABLE(kMuteFormantR, 0.90f);
SONARE_TUNABLE(kMuteFormantGain, 3.5f);
SONARE_TUNABLE(kMuteScoop, 0.45f);  // how much direct signal the mute removes
SONARE_TUNABLE(kMuteMixMax, 0.9f);

// --- 4c half-valve (only when params.half_valve > 0) ---
// Extra in-loop loss (a stuffier, more damped bore) and a small loop detune (the
// unstable, pitch-ambiguous half-valve wobble).
SONARE_TUNABLE(kHalfValveLossMax, 0.05f);
SONARE_TUNABLE(kHalfValveDetune, 0.006f);

// --- 4d dynamic (2-DOF) lip (only when params.dynamic_lip > 0) ---
// The transverse second lip mode sits above the note; its coupling adds a
// livelier buzz. Constant-Q like the primary lip.
SONARE_TUNABLE(kLip2Mult, 2.0f);
SONARE_TUNABLE(kLip2Q, 7.0f);
SONARE_TUNABLE(kLip2Couple, 1.5f);

}  // namespace

void BrassVoiceCore::start(const BrassPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float srf = static_cast<float>(sr);
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;
  breath_.releasing = false;
  breath_.level = 0.0f;
  lp_state_ = 0.0f;
  bore_.out = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  lip_x1_ = 0.0f;
  lip_x2_ = 0.0f;
  lip_z1_ = 0.0f;
  lip_z2_ = 0.0f;

  const float f0 = note_to_hz(note);
  bore_f0_ = f0;
  // Brass bore: a positive-feedback comb of the whole period, so the resonances
  // land on the full harmonic series (f0, 2f0, 3f0 …) a bell/mouthpiece-corrected
  // brass tube radiates. The lip valve buzzes the fundamental; the tube
  // reinforces every harmonic.
  const float period = srf / std::max(1.0f, f0);
  // Lengthen the loop a touch so the outward-striking sharpness lands on pitch.
  bore_.period = period * kPitchCorrect;
  sign_ = 1.0f;

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);
  // A fresh note starts unmodulated; the matrix re-sets the offsets on its
  // first render, and a voice with no excitation route never touches them.
  excite_.force01_base = level;
  excite_.force_mod01 = 0.0f;
  excite_.bright_mod01 = 0.0f;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);
  mouth_scale_ = kMouthScale;

  // Lip resonator: a constant-Q bandpass (two poles at R e^{±jw_lip} with a
  // DC/Nyquist zero pair) tuned to the note, nudged by lip_tension. The pole
  // radius follows the note (constant Q) so the lip stays selective at low notes;
  // lower damping = a higher Q = a sharper, brassier buzz.
  const float damp = std::clamp(params.lip_damping, 0.0f, 1.0f);
  const float tension = std::clamp(params.lip_tension, 0.0f, 1.0f);
  lip_srf_ = srf;
  lip_q_ = kLipQMin + kLipQSpan * (1.0f - damp);
  lip_tune_ = 1.0f + kLipTuneSpan * (tension - 0.5f);
  tune_lip(f0);
  lip_ratio_ = 1.0f;
  lip_offset_ = kLipOffset;
  lip_couple_ = kLipCouple;

  // Bell loop lowpass: brightness -> pole a (y += (1-a)(x - y)); a conical brass
  // reflects a touch darker, so the bore shape is remembered for the live
  // mapping.
  conical_ = params.conical;
  // Read before the mapping below runs, because the mapping branches on it.
  bell_cutoff_hz_ = std::max(0.0f, params.bell_cutoff_hz);
  excite_.bright01_base = params.brightness;
  // Both axes are derived here and nowhere else. A note-on copy of the mapping
  // is free to drift from the control-rate one, and a single rounding apart is
  // enough for the first CC to move a sound the host did not ask to move.
  refresh_excitation_targets();
  snap_excitation();
  loss_gain_ = std::clamp(kLossBase - kLossSpan * std::clamp(params.damping, 0.0f, 1.0f),
                          kLossFloor, kLossCeil);

  // In-loop DC blocker pole: the corner tracks f0 (floored) so its phase lead is
  // a bounded fraction of the period at every note.
  const float hp_corner = std::max(kDcCornerFloorHz, kDcCornerFracF0 * f0);
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * hp_corner / sr);

  // Tuning compensation: one feedback register (bore_.out is consumed one sample
  // after it is produced), the bell lowpass's phase delay (a lag that lengthens
  // the loop), and the DC blocker's phase LEAD at the fundamental (which shortens
  // it and would otherwise sharpen the low range). The lag and lead enter comp
  // with opposite signs.
  const float omega = kTwoPi / std::max(1.0f, bore_.period);
  const float tau_lp = onepole_group_delay_samples(1.0f - lp_alpha_, omega);
  const float sw = std::sin(omega);
  const float cw = std::cos(omega);
  const float phase_hp = std::atan2(sw, 1.0f - cw) - std::atan2(dc_r_ * sw, 1.0f - dc_r_ * cw);
  const float tau_hp = phase_hp / std::max(omega, 1.0e-6f);
  bore_.comp = 1.0f + tau_lp - kDcCompScale * tau_hp;

  // The bore delay line spans the whole slab, because the line length is what
  // bounds a downward bend and the clamp enforcing it saturates silently -- a
  // glide simply stops descending while the note keeps sounding. What the note's
  // own period still decides is how much of the line the onset seeds. The seed
  // IS the line's history, so the write position starts just past it and the
  // first traversal reads back over the seeded span exactly as it did when the
  // line was no longer than that span.
  bore_.configure(bore_.buffer, bore_.capacity, bore_.period, bore_.comp, 1.3f);

  // Contour + textures. Every seeded level is a per-sample draw voiced at
  // kLossVoicedSr, so each carries the noise law's gain.
  const float noise_gain = noise_gain_at_rate(sr);
  breath_.attack_coeff = ramp_coeff(params.attack_ms, sr);
  breath_.release_coeff = ramp_coeff(params.release_ms, sr);
  breath_noise_ = std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth * noise_gain;
  chiff_level_ = std::clamp(params.chiff, 0.0f, 1.0f) * kChiffDepth * noise_gain;
  chiff_coeff_ = ramp_coeff(params.chiff_ms, sr);
  const float peak_est = std::clamp(kPeakBase + kPeakTilt * std::log2(f0 / kPeakRefHz), 1.5f, 9.0f);
  output_scale_ = kOutputTargetPeak / peak_est;

  // Bell radiation highpass, normalised at the fundamental so the tilt is the
  // only thing it changes and the peak calibration above still holds.
  rad_state_ = 0.0f;
  rad_alpha_ = 0.0f;
  rad_scale_ = 1.0f;
  if (bell_cutoff_hz_ > 0.0f) {
    // One flare: what the bell radiates is what it did not reflect, so the two
    // filters run on the same pole and render() carries the gain that closes
    // |R|^2 + |T|^2 = 1. The level and register terms below are not part of
    // that identity and stay.
    rad_alpha_ = lp_alpha_;
  } else if (params.bell_radiation_hz > 0.0f) {
    rad_alpha_ = 1.0f - std::exp(-kTwoPi * std::min(params.bell_radiation_hz, 0.45f * srf) / srf);
  }
  if (rad_alpha_ > 0.0f) {
    const float pole = 1.0f - rad_alpha_;
    const float w0 = kTwoPi * f0 / srf;
    const float num = pole * 2.0f * std::fabs(std::sin(0.5f * w0));
    const float den = std::sqrt(1.0f - 2.0f * pole * std::cos(w0) + pole * pole);
    rad_scale_ = kBellRadiationMakeup * std::pow(den / std::max(num, 1.0e-6f), kBellRadiationNorm);
  }

  // Prompt speech: pre-fill the bore with a low-level seeded noise burst so the
  // lip resonator has an f0 component to lock onto rather than swelling up from
  // silence (a bandpass resonator ignores the breath DC). Past the seed the
  // line has to be cleared rather than left alone: it is a slab slot the
  // previous note wrote, and everything outside the seed is read before it is
  // written.
  const float prefill = kBorePrefill * breath_target_ * noise_gain;
  bore_.seed(prefill, noise_);
  drive_index_ = static_cast<uint64_t>(bore_.prefill_span);

  // --- off-by-default advanced physics (Phase 4). When off, render() takes the
  // linear branch untouched (bit-identical). ---

  // 4a: cuivré — a radiation-side level-preserving shock shaper. Off (0) ->
  // skipped. cuivre_scale_ normalises by the note's raw peak (~peak_est) so the
  // shaper sees a ~unit signal and the peak survives the reshaping.
  brassiness_ = std::clamp(params.brassiness, 0.0f, 1.0f);
  cuivre_dynamics_ = std::clamp(params.cuivre_dynamics, 0.0f, 1.0f);
  dyn_vel01_ = vel01;
  dyn_seat_ = level;
  cuivre_scale_ = peak_est;
  cuivre_inv_scale_ = 1.0f / std::max(0.5f, peak_est);
  const float cuivre_fc = std::clamp(kCuivreDriveRefHz / f0, 1.0f, kCuivreDriveRatioMax);
  cuivre_fc_sq_ = cuivre_fc * cuivre_fc;
  cuivre_drive_ = (1.0f + kCuivreDrive * brassiness_) * cuivre_fc_sq_;
  cuivre_inv_tanh_ = 1.0f / std::tanh(cuivre_drive_);
  cuivre_adaa_.reset(0.0f);

  // 4b: mute — a radiation-side resonant formant + scoop. Off (0) -> skipped.
  mute_ = std::clamp(params.mute, 0.0f, 1.0f);
  mute_x1_ = mute_x2_ = mute_y1_ = mute_y2_ = 0.0f;
  if (mute_ > 0.0f) {
    const float fm = std::min(kMuteFormantHz, 0.45f * srf);
    const float wm = kTwoPi * fm / srf;
    mute_peak_a1_ = 2.0f * kMuteFormantR * std::cos(wm);
    mute_peak_a2_ = -kMuteFormantR * kMuteFormantR;
    mute_peak_b0_ = 1.0f - kMuteFormantR;
  }

  // 4c: half-valve — extra in-loop loss and a small loop detune. Off (0) ->
  // skipped (the loss factor stays 1 and the bore period is untouched).
  half_valve_ = std::clamp(params.half_valve, 0.0f, 1.0f);
  half_valve_loss_ = 1.0f - kHalfValveLossMax * half_valve_;
  if (half_valve_ > 0.0f) {
    bore_.period *= (1.0f + kHalfValveDetune * half_valve_);
  }

  // 4d: dynamic (2-DOF) lip — a second, higher lip resonance. Off (0) -> skipped.
  dyn_lip_ = std::clamp(params.dynamic_lip, 0.0f, 1.0f);
  lip2_x1_ = lip2_x2_ = lip2_z1_ = lip2_z2_ = 0.0f;
  if (dyn_lip_ > 0.0f) {
    const float f2 = std::min(std::min(f0 * lip_tune_, 0.45f * srf) * kLip2Mult, 0.45f * srf);
    float r2 = std::exp(-kPi * (f2 / kLip2Q) / srf);
    r2 = std::min(r2, 0.99995f);
    const float w2 = kTwoPi * f2 / srf;
    lip2_a1_ = 2.0f * r2 * std::cos(w2);
    lip2_a2_ = -r2 * r2;
    lip2_b0_ = 1.0f - r2;
    lip2_couple_ = kLip2Couple * dyn_lip_;
  }

  // 4e: lip aperture — the lips as a one-sided valve instead of a symmetric
  // clamp on a reflection coefficient. Off (0) -> the symmetric path is taken
  // and the render is bit-identical.
  lip_aperture_ = std::clamp(params.lip_aperture, 0.0f, 1.0f);

  // 4f: amplitude-dependent propagation speed. Off (0) -> the bore delay is read
  // at the pitch ratio alone and the render is bit-identical.
  bore_nonlinearity_ = std::clamp(params.bore_nonlinearity, 0.0f, 1.0f);
}

void BrassVoiceCore::tune_lip(float f0) noexcept {
  // Constant Q: the pole radius follows the note so the lip stays as selective
  // at the bottom of the range as at the top.
  const float f_lip = std::min(f0 * lip_tune_, 0.45f * lip_srf_);
  float lip_r = std::exp(-kPi * (f_lip / lip_q_) / lip_srf_);
  lip_r = std::min(lip_r, 0.99995f);
  const float w = kTwoPi * f_lip / lip_srf_;
  lip_a1_ = 2.0f * lip_r * std::cos(w);
  lip_a2_ = -lip_r * lip_r;
  lip_b0_ = 1.0f - lip_r;  // peak gain ~unity; absolute gain absorbed by lip_couple_
  // The second lip resonance rides on the first, so it moves with it rather
  // than being left behind on the old note. Skipped while dyn_lip_ is still
  // zero, which is the case on the note-on call: start() sets it below and
  // tunes the second resonator itself, so that path is untouched.
  if (dyn_lip_ > 0.0f) {
    const float f2 = std::min(f_lip * kLip2Mult, 0.45f * lip_srf_);
    float r2 = std::exp(-kPi * (f2 / kLip2Q) / lip_srf_);
    r2 = std::min(r2, 0.99995f);
    const float w2 = kTwoPi * f2 / lip_srf_;
    lip2_a1_ = 2.0f * r2 * std::cos(w2);
    lip2_a2_ = -r2 * r2;
    lip2_b0_ = 1.0f - r2;
  }
}

void BrassVoiceCore::retune(float pitch_ratio) noexcept {
  // The bore follows a bend on its own -- the delay is divided by the ratio
  // every sample -- but the lip resonance is a filter tuned once at note-on, so
  // without this it stays on the old note and drags the sounding pitch back
  // toward it. Measured on a held bend, the lip gives back more than half the
  // interval and the response stops being monotone at the top of it.
  if (lip_srf_ <= 0.0f || lip_q_ <= 0.0f) return;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  tune_lip(bore_f0_ * ratio);
  lip_ratio_ = ratio;
}

float BrassVoiceCore::breath_swell() const noexcept {
  const float live =
      std::clamp((breath_target_ * breath_.level - kBreathBase) / kBreathSpan, 0.0f, 1.0f);
  return std::max(0.0f, live - dyn_seat_);
}

float BrassVoiceCore::played_dynamic() const noexcept {
  return std::clamp(dyn_vel01_ + breath_swell(), 0.0f, 1.0f);
}

float BrassVoiceCore::render(float pitch_ratio) noexcept {
  if (bore_.buffer == nullptr || bore_.capacity < 8) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // The bore follows the bend every sample below, but the lip resonance is a
  // filter, so without this it stays on the note it was tuned at and drags the
  // sounding pitch back toward it -- a two-semitone bend arrives as about 0.8,
  // and stops being monotone at the top. The factor carries vibrato, drift and
  // the patch's own pitch offset as well as the wheel, so a patch modulating
  // past the tolerance re-tunes with nothing bending it -- the lip tracks the
  // sounding pitch, which is the same physics a bend asks for.
  if (std::fabs(ratio - lip_ratio_) > kLipRetuneTolerance) retune(ratio);

  // Live control: ramp the steady breath / bell brightness toward their CC
  // targets (control-rate host updates, audio-rate smoothing -> no zipper).
  breath_target_ += ctrl_coeff_ * (breath_ctrl_target_ - breath_target_);
  lp_alpha_ += ctrl_coeff_ * (lp_alpha_target_ - lp_alpha_);

  // Mouth pressure contour: ramp toward the target (1 while blowing, 0 once the
  // player tongues off), then the steady breath plus its turbulence and the
  // onset chiff, scaled into the mouthpiece.
  breath_.advance();
  float breath = breath_target_ * breath_.level;
  if (breath_noise_ > 0.0f) {
    breath += breath * breath_noise_ * noise_.bipolar_at(drive_index_);
  }
  if (chiff_level_ > 1.0e-4f) {
    breath += chiff_level_ * breath_target_ * noise_.bipolar_at(drive_index_ + 1u);
    chiff_level_ -= chiff_coeff_ * chiff_level_;
  }
  const float mouth = mouth_scale_ * breath;

  // Bell reflection from the previous bore output: one-pole loss lowpass, the
  // loss gain and the topology sign.
  lp_state_ += lp_alpha_ * (bore_.out - lp_state_);
  float refl = sign_ * loss_gain_ * lp_state_;
  // Half-valve (gated): a stuffier, lossier bore.
  if (half_valve_ > 0.0f) refl *= half_valve_loss_;

  // Lip valve (resonant, outward-striking): the pressure difference across the
  // lips drives the resonant lip, and its displacement gates the mouth pressure
  // into the bore. The negative coupling sign is the outward-striking behaviour
  // that locks the buzz to the fundamental.
  const float dp = refl - mouth;
  const float x = lip_resonator(dp);
  float inj;
  if (lip_aperture_ > 0.0f) {
    // 4e: a swinging door. The displacement moves an opening rather than a
    // reflection coefficient, so it clamps at both ends — shut at 0, fully open
    // at 1 — and the flow through it follows Bernoulli. The clamp is what bounds
    // the loop on this branch: the flow grows as the aperture to the power of
    // three halves, so nothing downstream would.
    float h = lip_aperture_ + lip_couple_ * x;
    // Dynamic (2-DOF) lip (gated): the transverse second mode couples in.
    if (dyn_lip_ > 0.0f) h += lip2_couple_ * lip_resonator2(dp);
    h = std::clamp(h, 0.0f, 1.0f);
    // Mouthpiece to bore. This direction is what makes the displacement drive
    // the loop; the reflection-coefficient form below reaches the same sign by
    // multiplying two negatives, which is why the two arguments differ.
    const float dp_phys = mouth - refl;
    const float flow = h * std::copysign(std::sqrt(std::fabs(dp_phys)), dp_phys);
    inj = mouth + kLipFlowScale * flow;
  } else {
    // The symmetric path: the displacement modulates a reflection coefficient,
    // and its [-1,1] clamp is what bounds the loop here.
    float lip_coeff = lip_offset_ - lip_couple_ * x;
    if (dyn_lip_ > 0.0f) lip_coeff -= lip2_couple_ * lip_resonator2(dp);
    if (lip_coeff < -1.0f) lip_coeff = -1.0f;
    if (lip_coeff > 1.0f) lip_coeff = 1.0f;
    inj = mouth + dp * lip_coeff;
  }

  // DC-block the injection so the driven positive-feedback loop sheds the breath
  // DC without colouring the tone.
  const float dc = inj - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = inj;
  dc_y1_ = dc;

  // Advance the bore delay line: write the DC-blocked injection, read the delayed
  // pressure returning from the bell.
  //
  // 4f (gated): amplitude-dependent propagation speed. A pressure peak travels
  // faster than a trough, so the whole bore delay shortens where the pressure is
  // high — one combined variable delay standing in for the cascade of per-section
  // ones. The delay is read as period/ratio, so scaling the ratio up is how a
  // shorter delay reaches it without a second delay line. The modulator is the
  // bell's own reflection lowpass rather than the raw bore output: the pressure
  // wave in the bore is low-frequency dominated, and an unconditioned modulator
  // spends the interpolator's error on content that is about to be radiated
  // rather than reflected.
  float bore_ratio = ratio;
  if (bore_nonlinearity_ > 0.0f) {
    // kBoreSpeedSpan is the pressure at fortissimo, but the loop holds one
    // pressure at every velocity (the amp VCA carries the dynamic), so the speed
    // law's pressure is the loop's times the played dynamic.
    const float p_hat = lp_state_ * cuivre_inv_scale_ * played_dynamic();
    bore_ratio = ratio * std::clamp(1.0f + kBoreSpeedSpan * bore_nonlinearity_ * p_hat, 0.5f, 1.5f);
  }
  bore_.advance(dc, bore_ratio);
  ++drive_index_;

  float outp = bore_.out;

  // Cuivré (gated): the amplitude-dependent nonlinear wave steepening. The shaper
  // reshapes the normalised bore output through an asymmetric tanh shock front,
  // blooming the upper harmonics. Output-side (radiation) so it cannot
  // destabilise the loop — the practical bounded form of the shock (cf. the reed's
  // growth cone). The tanh is antialiased with first-order ADAA so the bloomed
  // upper harmonics do not fold back in the high register.
  if (brassiness_ > 0.0f) {
    float b_eff = brassiness_;
    float drive = cuivre_drive_;
    float inv_tanh = cuivre_inv_tanh_;
    if (cuivre_dynamics_ > 0.0f) {
      // Dynamic brassiness: the played dynamic scales the effective steepening.
      // The base is the note-on velocity (the amp VCA carries the loudness, so
      // the self-limiting mouth pressure cannot be the source); the breath
      // contour ramps it in over the attack, and a live CC2 swell above the
      // seated breath level adds on top. Squaring it makes the shock form
      // superlinearly with the dynamic, so a soft note stays round and the brassy
      // bloom concentrates near ff.
      const float dyn = std::clamp(dyn_vel01_ * breath_.level + breath_swell(), 0.0f, 1.0f);
      const float shaped_dyn = dyn * dyn;
      b_eff = std::clamp(brassiness_ * ((1.0f - cuivre_dynamics_) +
                                        cuivre_dynamics_ * kCuivreDynGain * shaped_dyn),
                         0.0f, 1.0f);
      drive = (1.0f + kCuivreDrive * b_eff) * cuivre_fc_sq_;
      inv_tanh = 1.0f / std::tanh(drive);
    }
    const float xn = outp * cuivre_inv_scale_;  // normalise to ~[-1,1]
    // Asymmetric shock: the |x| term steepens the front (even harmonics), tanh
    // bounds it; rescaling by 1/tanh(drive) keeps the full-scale peak so the
    // shaper brightens without crushing the level.
    const float xa = xn + kCuivreAsym * xn * std::fabs(xn);
    const float shaped = cuivre_adaa_.process(drive * xa) * inv_tanh * cuivre_scale_;
    outp += b_eff * kCuivreMixMax * (shaped - outp);
  }

  // Bell radiation (gated): the part the bell did not reflect. After the shock
  // shaper, which steepens inside the bore, and before the mute, which sits on
  // the bell's mouth.
  if (bell_cutoff_hz_ > 0.0f) {
    // The reflection's own smoothed pole, read live, so a moving controller can
    // never leave the two halves of the bell on different corners. The 1/sqrt
    // is what makes this highpass the power complement of that lowpass: for a
    // one-pole pair, 1 - |a/(1-pz^-1)|^2 is exactly |sqrt(p)(1-z^-1)/(1-pz^-1)|^2,
    // and the plain difference below carries p rather than sqrt(p).
    rad_alpha_ = lp_alpha_;
    const float pole = std::max(1.0f - rad_alpha_, 1.0e-4f);
    rad_state_ += rad_alpha_ * (outp - rad_state_);
    outp = rad_scale_ * (outp - rad_state_) / std::sqrt(pole);
  } else if (rad_alpha_ > 0.0f) {
    rad_state_ += rad_alpha_ * (outp - rad_state_);
    outp = rad_scale_ * (outp - rad_state_);
  }

  // Mute (gated): a resonant upper formant plus a scoop of the direct low-mid,
  // the nasal honk of a straight/cup mute on the bell.
  if (mute_ > 0.0f) {
    const float peak =
        mute_peak_b0_ * (outp - mute_x2_) + mute_peak_a1_ * mute_y1_ + mute_peak_a2_ * mute_y2_;
    mute_x2_ = mute_x1_;
    mute_x1_ = outp;
    mute_y2_ = mute_y1_;
    mute_y1_ = peak;
    const float muted = outp * (1.0f - kMuteScoop) + peak * kMuteFormantGain;
    const float wet = mute_ * kMuteMixMax;
    outp += wet * (muted - outp);
  }

  return output_scale_ * outp;
}

float BrassVoiceCore::lip_resonator(float dp) noexcept {
  // Bandpass biquad: H(z) = b0 (1 - z^-2) / (1 - a1 z^-1 - a2 z^-2). The DC /
  // Nyquist zero pair keeps the steady breath from driving the resonance, so only
  // the f0 content of the loop rings the lip — the buzzing mass-spring.
  const float y = lip_b0_ * (dp - lip_x2_) + lip_a1_ * lip_z1_ + lip_a2_ * lip_z2_;
  lip_x2_ = lip_x1_;
  lip_x1_ = dp;
  lip_z2_ = lip_z1_;
  lip_z1_ = y;
  return y;
}

float BrassVoiceCore::lip_resonator2(float dp) noexcept {
  // Second (transverse) lip mode, same DC-zeroed bandpass form, tuned above the
  // note (the 2-DOF lip's higher resonance).
  const float y = lip2_b0_ * (dp - lip2_x2_) + lip2_a1_ * lip2_z1_ + lip2_a2_ * lip2_z2_;
  lip2_x2_ = lip2_x1_;
  lip2_x1_ = dp;
  lip2_z2_ = lip2_z1_;
  lip2_z1_ = y;
  return y;
}

void BrassVoiceCore::set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept {
  excite_.set_base(base, present & ~kAxisBrightness);
  if ((present & kAxisBrightness) != 0u) {
    // bell_alpha_for_brightness clamps its own argument.
    excite_.bright01_base = base.brightness;
  }
  refresh_excitation_targets();
}

void BrassVoiceCore::set_excitation_mod(const ExcitationAxes& offsets) noexcept {
  excite_.set_mod(offsets);
  refresh_excitation_targets();
}

void BrassVoiceCore::refresh_excitation_targets() noexcept {
  const float b = std::clamp(excite_.force01_base + excite_.force_mod01, 0.0f, 1.0f);
  breath_ctrl_target_ = kBreathBase + kBreathSpan * b;
  // bell_alpha_for_brightness clamps its own argument.
  lp_alpha_target_ = bell_alpha_for_brightness(excite_.bright01_base + excite_.bright_mod01);
}

float BrassVoiceCore::bell_alpha_for_brightness(float bright01) const noexcept {
  if (bell_cutoff_hz_ > 0.0f) {
    // One flare, named in hertz. Brightness opens it and a conical bore sits
    // lower, both as octave offsets on the same corner rather than as biases on
    // a pole — which is what makes the corner mean the same thing at every rate
    // and leaves nothing for the two halves of the bell to disagree about.
    float octaves = kBellBrightOct * (std::clamp(bright01, 0.0f, 1.0f) - 0.5f);
    if (conical_) octaves -= kConicalOct;
    const float f_eff = std::min(bell_cutoff_hz_ * std::exp2(octaves), 0.45f * lip_srf_);
    return 1.0f - std::exp(-kTwoPi * f_eff / lip_srf_);
  }
  float a = (1.0f - std::clamp(bright01, 0.0f, 1.0f)) * kBellPoleSpan;
  if (conical_) a = std::min(a + kConicalDarken, 0.95f);
  // Voiced at kLossVoicedSr like every other bell and bridge in the bank. The
  // lip resonator is already quoted in Hz and dominates the timbre, so this one
  // is masked rather than absent — and a masked pole still moves its corner with
  // the rate, which is what the mapping removes.
  return 1.0f - loss_pole_at_rate(a, lip_srf_);
}

void BrassVoiceCore::snap_excitation() noexcept {
  breath_target_ = breath_ctrl_target_;
  lp_alpha_ = lp_alpha_target_;
}

void BrassVoiceCore::release() noexcept { breath_.release(); }

void BrassVoiceCore::kill() noexcept {
  breath_.level = 0.0f;
  lp_state_ = 0.0f;
  rad_state_ = 0.0f;
  bore_.out = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  chiff_level_ = 0.0f;
  lip_x1_ = 0.0f;
  lip_x2_ = 0.0f;
  lip_z1_ = 0.0f;
  lip_z2_ = 0.0f;
  mute_x1_ = 0.0f;
  mute_x2_ = 0.0f;
  mute_y1_ = 0.0f;
  mute_y2_ = 0.0f;
  lip2_x1_ = 0.0f;
  lip2_x2_ = 0.0f;
  lip2_z1_ = 0.0f;
  lip2_z2_ = 0.0f;
  breath_.releasing = true;
}

}  // namespace sonare::midi::synth
