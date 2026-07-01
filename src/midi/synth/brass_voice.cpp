#include "midi/synth/brass_voice.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Mouth-pressure scale: the player's lung pressure enters the mouthpiece. The
// lips buzz only above a threshold loop gain, and mouth * lip_couple is the
// active gain, so the mouth pressure must reach ~unity for the lips to speak —
// this scale brings the calibrated breath band up to that buzzing pressure.
constexpr float kMouthScale = 1.0f;

// Mouth-pressure calibration: the exposed breath range lands in the strong
// buzzing band ([~0.72, ~1.0]) — the knobs colour the tone, they do not gate it
// on and off. Dynamic LOUDNESS comes from the voice's velocity / amp VCA around
// this core, not from pushing the breath, so breath varies only mildly with the
// note. breath = base + span*level.
constexpr float kBreathBase = 0.72f;
constexpr float kBreathSpan = 0.28f;

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
constexpr float kLipOffset = -0.1f;
constexpr float kLipCouple = 4.5f;
// Lip resonator quality factor from lip_damping, held CONSTANT-Q (bandwidth
// proportional to the note) so the lip stays selective at low notes — a
// fixed-radius resonator is wider than the fundamental in the tuba range and
// lets the octave win, jumping the register. A tight lip is a sharp, high-Q
// resonance (a bright, brassy buzz); a loose lip is broader and lower-Q (a
// mellow tone). Q = min + span*(1 - lip_damping).
constexpr float kLipQMin = 8.0f;
constexpr float kLipQSpan = 22.0f;
// Lip tension detunes the lip resonance a little above / below the note (the
// embouchure centre). Small — a few percent.
constexpr float kLipTuneSpan = 0.04f;  // f_lip = f0 * (1 + span*(tension - 0.5))
// Pitch correction: an outward-striking lip oscillates just ABOVE its resonance
// (Fletcher 1979), so the played note lands ~0.8% sharp of the bore/lip lock;
// the loop delay is lengthened to bring it back onto pitch.
constexpr float kPitchCorrect = 1.0075f;

// Bell reflection loss from damping: < 1 so the loop is stable (the breath
// replenishes it). Low damping = a purer, more sustained bore.
constexpr float kLossBase = 0.995f;
constexpr float kLossSpan = 0.08f;
constexpr float kLossFloor = 0.85f;
constexpr float kLossCeil = 0.999f;

// Bell loop-lowpass depth: brightness maps to the one-pole pole (a brighter bell
// reflects more upper partials). The conical bias darkens a conical brass.
constexpr float kBellPoleSpan = 0.7f;
constexpr float kConicalDarken = 0.12f;  // extra pole for conical (horn / tuba)

// Live-control smoothing time (ms).
constexpr float kControlSmoothMs = 8.0f;

// Breath turbulence depth (a light seeded jitter on the mouth pressure).
constexpr float kBreathNoiseDepth = 0.08f;

// Onset chiff depth (the tonguing "speak" noise burst) and the level of the
// seeded burst pre-filled into the bore so the note speaks promptly.
constexpr float kChiffDepth = 0.5f;
constexpr float kBorePrefill = 0.03f;

// In-loop DC-blocker corner (~10 Hz): the positive-feedback comb has a DC mode
// that does not radiate and the breath DC drives the lips, so the injection is
// DC-blocked before entering the bore.
constexpr float kDcCornerHz = 10.0f;

// Output trim: the driven loop settles with a raw bore peak that grows with the
// note (~2.3 at the bottom of the range to ~7 at the top), so the output scale is
// frequency-compensated to keep a forte note near a flat target peak across the
// keyboard. peak_raw ~= kPeakBase + kPeakTilt*log2(f0/kPeakRefHz).
constexpr float kOutputTargetPeak = 0.6f;
constexpr float kPeakBase = 2.33f;
constexpr float kPeakTilt = 0.93f;
constexpr float kPeakRefHz = 44.0f;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

}  // namespace

void BrassVoiceCore::start(const BrassPatchParams& params, double sample_rate, uint8_t note,
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
  lip_x1_ = 0.0f;
  lip_x2_ = 0.0f;
  lip_z1_ = 0.0f;
  lip_z2_ = 0.0f;

  const float f0 = note_to_hz(note);
  // Brass bore: a positive-feedback comb of the whole period, so the resonances
  // land on the full harmonic series (f0, 2f0, 3f0 …) a bell/mouthpiece-corrected
  // brass tube radiates. The lip valve buzzes the fundamental; the tube
  // reinforces every harmonic.
  const float period = srf / std::max(1.0f, f0);
  // Lengthen the loop a touch so the outward-striking sharpness lands on pitch.
  bore_period_ = period * kPitchCorrect;
  sign_ = 1.0f;

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);
  breath_target_ = kBreathBase + kBreathSpan * level;
  breath_ctrl_target_ = breath_target_;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);
  mouth_scale_ = kMouthScale;

  // Lip resonator: a constant-Q bandpass (two poles at R e^{±jw_lip} with a
  // DC/Nyquist zero pair) tuned to the note, nudged by lip_tension. The pole
  // radius follows the note (constant Q) so the lip stays selective at low notes;
  // lower damping = a higher Q = a sharper, brassier buzz.
  const float damp = std::clamp(params.lip_damping, 0.0f, 1.0f);
  const float tension = std::clamp(params.lip_tension, 0.0f, 1.0f);
  const float f_lip = std::min(f0 * (1.0f + kLipTuneSpan * (tension - 0.5f)), 0.45f * srf);
  const float q = kLipQMin + kLipQSpan * (1.0f - damp);
  float lip_r = std::exp(-static_cast<float>(M_PI) * (f_lip / q) / srf);
  lip_r = std::min(lip_r, 0.99995f);
  const float w = kTwoPi * f_lip / srf;
  lip_a1_ = 2.0f * lip_r * std::cos(w);
  lip_a2_ = -lip_r * lip_r;
  lip_b0_ = 1.0f - lip_r;  // peak gain ~unity; absolute gain absorbed by lip_couple_
  lip_offset_ = kLipOffset;
  lip_couple_ = kLipCouple;

  // Bell loop lowpass: brightness -> pole a (y += (1-a)(x - y)); a conical brass
  // reflects a touch darker.
  float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * kBellPoleSpan;
  if (params.conical) a = std::min(a + kConicalDarken, 0.95f);
  lp_alpha_ = 1.0f - a;
  lp_alpha_target_ = lp_alpha_;
  loss_gain_ = std::clamp(kLossBase - kLossSpan * std::clamp(params.damping, 0.0f, 1.0f),
                          kLossFloor, kLossCeil);

  // In-loop DC blocker pole.
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * kDcCornerHz / sr);

  // Tuning compensation: one feedback register (bore_out_ is consumed one sample
  // after it is produced) plus the bell lowpass's phase delay at the resonant
  // frequency. Subtract from the loop delay.
  const float omega = kTwoPi / std::max(1.0f, bore_period_);
  const float tau_lp =
      std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
  comp_ = 1.0f + tau_lp;

  // Circular span sized for the whole loop period plus bend-down headroom and the
  // interpolator stencil margin.
  const float eff = std::max(2.0f, bore_period_ - comp_);
  const int span = static_cast<int>(eff * 1.3f) + 8;
  bore_size_ = std::min(capacity_, std::max(16, span));
  bore_write_ = 0;

  // Contour + textures.
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);
  breath_noise_ = std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth;
  chiff_level_ = std::clamp(params.chiff, 0.0f, 1.0f) * kChiffDepth;
  chiff_coeff_ = ramp_coeff(params.chiff_ms, sr);
  const float peak_est = std::clamp(kPeakBase + kPeakTilt * std::log2(f0 / kPeakRefHz), 1.5f, 9.0f);
  output_scale_ = kOutputTargetPeak / peak_est;

  // Prompt speech: pre-fill the bore with a low-level seeded noise burst so the
  // lip resonator has an f0 component to lock onto rather than swelling up from
  // silence (a bandpass resonator ignores the breath DC).
  const float prefill = kBorePrefill * breath_target_;
  if (bore_ != nullptr) {
    for (int i = 0; i < bore_size_; ++i) {
      bore_[static_cast<size_t>(i)] = prefill * noise_.bipolar_at(static_cast<uint64_t>(i));
    }
  }
  drive_index_ = static_cast<uint64_t>(bore_size_);
}

float BrassVoiceCore::render(float pitch_ratio) noexcept {
  if (bore_ == nullptr || bore_size_ < 8) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Live control: ramp the steady breath / bell brightness toward their CC
  // targets (control-rate host updates, audio-rate smoothing -> no zipper).
  breath_target_ += ctrl_coeff_ * (breath_ctrl_target_ - breath_target_);
  lp_alpha_ += ctrl_coeff_ * (lp_alpha_target_ - lp_alpha_);

  // Mouth pressure contour: ramp toward the target (1 while blowing, 0 once the
  // player tongues off), then the steady breath plus its turbulence and the
  // onset chiff, scaled into the mouthpiece.
  const float target = releasing_ ? 0.0f : 1.0f;
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  breath_level_ += coeff * (target - breath_level_);
  float breath = breath_target_ * breath_level_;
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
  lp_state_ += lp_alpha_ * (bore_out_ - lp_state_);
  const float refl = sign_ * loss_gain_ * lp_state_;

  // Lip valve (resonant, outward-striking): the pressure difference across the
  // lips drives the resonant lip, and its displacement modulates a reflection
  // coefficient (clamped to [-1,1]) that gates the mouth pressure into the bore —
  // the reed-style flow inj = mouth + dp*coeff, but with the coefficient being
  // the buzzing lip resonance rather than a memoryless table. The negative
  // coupling sign is the outward-striking behaviour that locks the buzz to the
  // fundamental. The [-1,1] clamp is the nonlinearity that keeps the loop bounded
  // and injects the harmonics (the octave and above) that a bare resonance lacks.
  const float dp = refl - mouth;
  const float x = lip_resonator(dp);
  float lip_coeff = lip_offset_ - lip_couple_ * x;
  if (lip_coeff < -1.0f) lip_coeff = -1.0f;
  if (lip_coeff > 1.0f) lip_coeff = 1.0f;
  const float inj = mouth + dp * lip_coeff;

  // DC-block the injection so the driven positive-feedback loop sheds the breath
  // DC without colouring the tone.
  const float dc = inj - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = inj;
  dc_y1_ = dc;

  // Advance the bore delay line: write the DC-blocked injection, read the delayed
  // pressure returning from the bell.
  const float delay =
      std::clamp(bore_period_ / ratio - comp_, 1.0f, static_cast<float>(bore_size_ - 4));
  bore_out_ = rt::lagrange3_fractional_delay(bore_, static_cast<size_t>(bore_size_), bore_write_,
                                             static_cast<int>(delay * 256.0f), dc);
  ++drive_index_;

  return output_scale_ * bore_out_;
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

void BrassVoiceCore::set_breath(float breath01) noexcept {
  const float b = breath01 < 0.0f ? 0.0f : (breath01 > 1.0f ? 1.0f : breath01);
  breath_ctrl_target_ = kBreathBase + kBreathSpan * b;
}

void BrassVoiceCore::set_brightness(float bright01) noexcept {
  const float br = bright01 < 0.0f ? 0.0f : (bright01 > 1.0f ? 1.0f : bright01);
  lp_alpha_target_ = 1.0f - (1.0f - br) * kBellPoleSpan;
}

void BrassVoiceCore::snap_brass_control() noexcept {
  breath_target_ = breath_ctrl_target_;
  lp_alpha_ = lp_alpha_target_;
}

void BrassVoiceCore::release() noexcept { releasing_ = true; }

void BrassVoiceCore::kill() noexcept {
  breath_level_ = 0.0f;
  lp_state_ = 0.0f;
  bore_out_ = 0.0f;
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  chiff_level_ = 0.0f;
  lip_x1_ = 0.0f;
  lip_x2_ = 0.0f;
  lip_z1_ = 0.0f;
  lip_z2_ = 0.0f;
  releasing_ = true;
}

}  // namespace sonare::midi::synth
