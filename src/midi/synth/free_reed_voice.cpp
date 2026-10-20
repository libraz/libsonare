#include "midi/synth/free_reed_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/string_loop.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kA4Hz;
using sonare::constants::kTwoPi;

// Musette detune span (cents) between the two tongues: detune 0 collapses to a
// single tongue (the second oscillator is skipped entirely, bit-identical); the
// knob then opens from a barely-wet couple of cents to a wide wet-tuned
// accordion shimmer.
SONARE_TUNABLE(kDetuneMinCents, 3.0f);
SONARE_TUNABLE(kDetuneSpanCents, 12.0f);

// Tongue nonlinearity calibration. The tongue swinging INTO the slot chokes the
// airflow harder than it releases it on the way out, so the saturator's gain is
// biased per half-cycle: asymmetry from reed_stiffness skews the waveform (the
// even-harmonic "free reed" bias), drive pushes the saturator toward its knee
// for the buzzy odd-harmonic edge. Both spans keep tanh well inside float range.
// All five reach only the shaped saw, which slot_duty > 0 replaces; every
// shipped patch sets one, so a fit over these five moves nothing.
SONARE_TUNABLE(kAsymBase, 0.15f);
SONARE_TUNABLE(kAsymSpan, 0.45f);
SONARE_TUNABLE(kDriveBase, 1.2f);
SONARE_TUNABLE(kDriveStiffSpan, 2.4f);
SONARE_TUNABLE(kDriveBreathSpan, 1.2f);

// Body/radiation one-pole corner (Hz): brightness sweeps the reed-plate /
// cavity roll-off log-linearly from a mellow reed organ to a bright buzzy
// harmonica. Clamped below Nyquist at start().
SONARE_TUNABLE(kBodyMinHz, 600.0f);
SONARE_TUNABLE(kBodyMaxHz, 9000.0f);

// Leakage air hiss depth at breath_noise = 1 (added before the body lowpass so
// the hiss is coloured by the same radiation roll-off as the reed).
SONARE_TUNABLE(kBreathNoiseDepth, 0.12f);

// Output trim: the saturated tongue sits near +/-1, so the bellows drive level
// maps a forte note into the other engines' range and a soft note well below.
SONARE_TUNABLE(kOutputMin, 0.3f);
SONARE_TUNABLE(kOutputSpan, 0.5f);

// Slot flow (gated). The return pass is the narrower of the two on every
// reference fitted; held here rather than per patch because no reference moved
// it. The make-up brings the radiated pair back to the saw shaper's level.
SONARE_TUNABLE(kSlotReturnWidth, 0.75f);
SONARE_TUNABLE(kSlotMakeup, 0.55f);

// Slot swing (gated), in plate thicknesses: the tongue's rest height over the
// plate, the lift past which the slot's own area caps the flow, and the share the
// far side passes. Set on the accordion reference's odd-over-even bass ladder.
SONARE_TUNABLE(kSwingRestOffset, 0.3f);
SONARE_TUNABLE(kSwingSaturation, 1.0f);
SONARE_TUNABLE(kSwingReturnFlow, 0.3f);
// Period steps for the swing flow's mean, taken once per note.
constexpr int kSwingMeanSteps = 256;

// Live-control glide (ms) toward a moved axis, matching the other
// continuously-excited engines.
SONARE_TUNABLE(kControlSmoothMs, 8.0f);

// Gap at which the ramp is declared arrived and the per-sample re-cut stops.
// Below an axis step no controller can express, so settling costs nothing
// audible and buys back the pow()/exp() the re-cut spends.
constexpr float kCtrlSettle = 1.0e-6f;

/// One flow hump: a quartic bump of fractional @p width centred at @p centre,
/// zero in value and slope at both edges so the source rolls off at 18 dB per
/// octave and folds little back under Nyquist.
float flow_hump(float phase, float centre, float width) noexcept {
  float d = phase - centre;
  d -= std::floor(d + 0.5f);
  const float u = 2.0f * d / width;
  if (u <= -1.0f || u >= 1.0f) return 0.0f;
  const float v = 1.0f - u * u;
  return v * v;
}

float slot_flow(float phase, float duty, float return_amount, float gap,
                float return_width) noexcept {
  return flow_hump(phase, 0.0f, duty) + return_amount * flow_hump(phase, gap, return_width);
}

/// The gap a tongue swinging @p swing plate thicknesses opens at @p phase: linear
/// in the lift, capped by the slot, choked inside the plate, partly open beyond it.
float swing_flow(float phase, float swing) noexcept {
  const float lift = swing * std::sin(static_cast<float>(kTwoPi) * phase) + kSwingRestOffset;
  return std::clamp(lift, 0.0f, kSwingSaturation) +
         kSwingReturnFlow * std::clamp(-lift - 1.0f, 0.0f, kSwingSaturation);
}

float swing_mean(float swing) noexcept {
  float sum = 0.0f;
  for (int i = 0; i < kSwingMeanSteps; ++i) {
    sum += swing_flow((static_cast<float>(i) + 0.5f) / static_cast<float>(kSwingMeanSteps), swing);
  }
  return sum / static_cast<float>(kSwingMeanSteps);
}

float effective_radiation_halfangle(float halfangle, float ratio, float sample_rate) noexcept {
  // A first difference has no useful fundamental calibration below 20 Hz or
  // above Nyquist. Keeping the effective angle in that physical interval also
  // prevents an aliased pitch from landing on a numerical zero of sin().
  const float nyquist_halfangle = 0.25f * static_cast<float>(kTwoPi);
  const float minimum_halfangle =
      std::min(0.5f * static_cast<float>(kTwoPi) * 20.0f / sample_rate, nyquist_halfangle);
  return std::clamp(std::abs(halfangle * ratio), minimum_halfangle, nyquist_halfangle);
}

float radiation_norm(float halfangle, float ratio, float sample_rate) noexcept {
  const float effective_halfangle = effective_radiation_halfangle(halfangle, ratio, sample_rate);
  return 1.0f / std::max(2.0f * std::abs(std::sin(effective_halfangle)), 1.0e-6f);
}

float radiation_mix_norm(float radiation, float halfangle, float ratio,
                         float sample_rate) noexcept {
  if (radiation <= 0.0f || radiation >= 1.0f) return 1.0f;
  const float theta = effective_radiation_halfangle(halfangle, ratio, sample_rate);
  const float dry = 1.0f - radiation;
  const float magnitude_sq =
      dry * dry + radiation * radiation + 2.0f * dry * radiation * std::sin(theta);
  return 1.0f / std::sqrt(std::max(magnitude_sq, 1.0e-12f));
}

}  // namespace

void FreeReedVoiceCore::start(const FreeReedPatchParams& params, double sample_rate, uint8_t note,
                              Velocity16 velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  sample_rate_ = sr;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 1;
  body_state_ = 0.0f;

  base_freq_hz_ = note_to_hz(note);
  // Base phase increments (cycles per sample); render() scales them by the live
  // pitch ratio so bend/drift keep the musette pair's ratio intact.
  inc_a_ = static_cast<float>(base_freq_hz_ / sr);

  // Musette: a second tongue a few cents sharp, and optionally a third as far
  // flat. detune == 0 leaves no partner, so the single-tongue path never touches them.
  const float detune = std::clamp(params.detune, 0.0f, 1.0f);
  partner_count_ = detune > 0.0f ? (params.musette_flat ? 2 : 1) : 0;
  partner_level_ = std::clamp(params.musette_partner_level, 0.0f, 1.0f);
  tongue_mix_ = 1.0f / (1.0f + static_cast<float>(partner_count_) * partner_level_);
  phase_a_ = 0.0f;
  for (int i = 0; i < kMaxPartners; ++i) partners_[i] = Partner{};
  // The offset's cents fall as (A4/f0)^taper: 0 holds the cents, 1 the beat rate in Hz.
  const float taper = std::clamp(params.musette_taper, 0.0f, 1.0f);
  const float cents_a4 = kDetuneMinCents + kDetuneSpanCents * detune;
  const float cents = taper > 0.0f ? cents_a4 * std::pow(kA4Hz / base_freq_hz_, taper) : cents_a4;
  for (int i = 0; i < partner_count_; ++i) {
    partners_[i].inc = inc_a_ * cents_to_ratio(i == 0 ? cents : -cents);
    // Decorrelate the tongues' start (a real accordion's tongues never speak in
    // phase); seeded, so the offset is deterministic per voice.
    partners_[i].phase = noise_.unipolar_at(static_cast<uint64_t>(i));
  }

  // Bellows drive level: steady pressure blended with the struck velocity.
  const float vel01 = velocity.f7() / 127.0f;
  const float vel_to_breath = std::clamp(params.vel_to_breath, 0.0f, 1.0f);
  const float level = std::clamp(
      (1.0f - vel_to_breath) * params.breath_pressure + vel_to_breath * vel01, 0.0f, 1.0f);

  // Tongue nonlinearity: stiffness skews the in/out-of-slot asymmetry, and the
  // drive (stiffness plus bellows pressure) sets how hard the saturator works.
  const float stiffness = std::clamp(params.reed_stiffness, 0.0f, 1.0f);
  asymmetry_ = kAsymBase + kAsymSpan * stiffness;
  drive_stiff_ = kDriveBase + kDriveStiffSpan * stiffness;
  drive_ = drive_stiff_ + kDriveBreathSpan * level;

  // Body lowpass pole from brightness (log-swept corner, clamped under Nyquist).
  const float brightness = std::clamp(params.brightness, 0.0f, 1.0f);
  const float corner = std::min(kBodyMinHz * std::pow(kBodyMaxHz / kBodyMinHz, brightness),
                                0.45f * static_cast<float>(sr));
  body_alpha_ = 1.0f - std::exp(-kTwoPi * corner / static_cast<float>(sr));

  // Slot flow (gated): the pair of humps the tongue passes on its way through
  // the slot, radiated as a small monopole.
  slot_duty_ = std::max(params.slot_duty, 0.0f);
  slot_return_ = std::max(params.slot_return, 0.0f);
  slot_return_width_ = slot_duty_ * kSlotReturnWidth;
  slot_gap_ = params.slot_gap;
  radiation_ = std::clamp(params.radiation, 0.0f, 1.0f);
  prev_flow_ = 0.0f;
  // The humps' own mean is the steady flow that holds the slot open and
  // radiates nothing. Subtracted in closed form: a quartic bump integrates to
  // 8/15 of its width, so the pair's mean is exact and needs no tracker.
  slot_mean_ = (8.0f / 15.0f) * (slot_duty_ + slot_return_ * slot_return_width_);
  // The swing is the patch's at A4 scaled 1/f0: tip amplitude follows tongue
  // length squared over thickness, which is what sets the pitch.
  swing_ = params.slot_swing > 0.0f ? params.slot_swing * kA4Hz / base_freq_hz_ : 0.0f;
  if (swing_ > 0.0f) slot_mean_ = swing_mean(swing_);
  // A first difference is the monopole's +6 dB/octave, and it is 2*sin(pi*f0/sr)
  // at the fundamental — small, and note-dependent. Normalising it away leaves
  // the tilt without the level or the register slope that come with it.
  radiation_halfangle_ = static_cast<float>(kTwoPi * base_freq_hz_ / (2.0 * sr));
  radiation_ratio_ = 1.0f;
  radiation_norm_ = radiation_norm(radiation_halfangle_, 1.0f, static_cast<float>(sr));
  radiation_mix_norm_ =
      radiation_mix_norm(radiation_, radiation_halfangle_, 1.0f, static_cast<float>(sr));
  for (int i = 0; i < partner_count_; ++i) {
    Partner& t = partners_[i];
    t.halfangle = 0.5f * static_cast<float>(kTwoPi) * t.inc;
    t.norm = radiation_norm(t.halfangle, 1.0f, static_cast<float>(sr));
    t.mix_norm = radiation_mix_norm(radiation_, t.halfangle, 1.0f, static_cast<float>(sr));
  }

  // The differentiator history is primed on the first render, after its live
  // pitch ratio is known. Keep the old zero history for a normal mono start.
  prev_flow_ = 0.0f;
  radiation_history_ready_ = false;

  // Contour + textures.
  bellows_.start(params.attack_ms, params.release_ms, sr);
  // The overshoot moves per octave from A4; a treble tongue follows the bellows without one.
  const float overshoot_db =
      params.onset_breath_overshoot_db > 0.0f && params.onset_breath_overshoot_key_db != 0.0f
          ? std::max(0.0f, params.onset_breath_overshoot_db + params.onset_breath_overshoot_key_db *
                                                                  std::log2(base_freq_hz_ / kA4Hz))
          : params.onset_breath_overshoot_db;
  bellows_.set_overshoot_at_velocity(overshoot_db, params.onset_breath_overshoot_vel_db, vel01,
                                     params.onset_breath_settle_ms, sr);
  // A per-sample draw voiced at kLossVoicedSr, so it carries the noise law's gain.
  breath_noise_ =
      std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth * noise_gain_at_rate(sr);
  // The radiated pair swings wider than the saturated saw it replaces, so the
  // patch gains stay comparable across the two shapers.
  slot_makeup_ = slot_duty_ > 0.0f ? kSlotMakeup : 1.0f;
  // A monopole radiates +6 dB per octave; the share kept rides on the note's base pitch.
  const float keytrack = std::clamp(params.radiation_keytrack, 0.0f, 1.0f);
  register_gain_ = slot_duty_ > 0.0f && radiation_ > 0.0f && keytrack > 0.0f
                       ? std::pow(base_freq_hz_ / kA4Hz, keytrack)
                       : 1.0f;
  output_scale_ = (kOutputMin + kOutputSpan * level) * slot_makeup_ * register_gain_;

  // Live axes start where the note put them, so nothing is armed and the
  // coefficients above stand until a control moves.
  excite_.force01_base = level;
  excite_.force_mod01 = 0.0f;
  force01_target_ = level;
  force01_ = level;
  excite_.bright01_base = brightness;
  excite_.bright_mod01 = 0.0f;
  bright01_target_ = brightness;
  bright01_ = brightness;
  ctrl_coeff_ = ramp_coeff(kControlSmoothMs, sr);
  excitation_live_ = false;
  excite_.remember_base();
}

void FreeReedVoiceCore::set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept {
  if ((present & kAxisForce) != 0u) hold_onset();
  excite_.set_base(base, present);
  refresh_excitation_targets();
}

void FreeReedVoiceCore::restore_excitation_base() noexcept {
  excite_.restore_base();
  refresh_excitation_targets();
}

void FreeReedVoiceCore::set_excitation_mod(const ExcitationAxes& offsets) noexcept {
  excite_.set_mod(offsets);
  refresh_excitation_targets();
}

void FreeReedVoiceCore::refresh_excitation_targets() noexcept {
  force01_target_ = std::clamp(excite_.force01_base + excite_.force_mod01, 0.0f, 1.0f);
  bright01_target_ = std::clamp(excite_.bright01_base + excite_.bright_mod01, 0.0f, 1.0f);
  if (force01_target_ != force01_ || bright01_target_ != bright01_) excitation_live_ = true;
}

void FreeReedVoiceCore::apply_excitation() noexcept {
  drive_ = drive_stiff_ + kDriveBreathSpan * force01_;
  output_scale_ = (kOutputMin + kOutputSpan * force01_) * slot_makeup_ * register_gain_;
  const float srf = static_cast<float>(sample_rate_);
  const float corner =
      std::min(kBodyMinHz * std::pow(kBodyMaxHz / kBodyMinHz, bright01_), 0.45f * srf);
  body_alpha_ = 1.0f - std::exp(-kTwoPi * corner / srf);
}

void FreeReedVoiceCore::advance_excitation() noexcept {
  force01_ += ctrl_coeff_ * (force01_target_ - force01_);
  bright01_ += ctrl_coeff_ * (bright01_target_ - bright01_);
  if (std::abs(force01_target_ - force01_) < kCtrlSettle &&
      std::abs(bright01_target_ - bright01_) < kCtrlSettle) {
    force01_ = force01_target_;
    bright01_ = bright01_target_;
    excitation_live_ = false;
  }
  apply_excitation();
}

void FreeReedVoiceCore::snap_excitation() noexcept {
  if (force01_ == force01_target_ && bright01_ == bright01_target_) return;
  force01_ = force01_target_;
  bright01_ = bright01_target_;
  excitation_live_ = false;
  apply_excitation();
}

float FreeReedVoiceCore::render(float pitch_ratio) noexcept {
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  if (excitation_live_) advance_excitation();

  // Bellows contour: one-pole ramp toward 1 while blowing, 0 once released.
  bellows_.advance();

  if (!radiation_history_ready_) {
    const float effective_frequency = base_freq_hz_ * ratio;
    const float nyquist = 0.5f * static_cast<float>(sample_rate_);
    if (slot_duty_ > 0.0f &&
        (partner_count_ > 0 || effective_frequency < 20.0f || effective_frequency > nyquist)) {
      // This is deliberately before phase advance: the first difference must
      // compare the first rendered phase against the phase that preceded it.
      prev_flow_ = slot_flow_at(phase_a_) - slot_mean_;
      for (int i = 0; i < partner_count_; ++i) {
        Partner& t = partners_[i];
        t.prev_flow = slot_flow_at(t.phase) - slot_mean_;
      }
    }
    radiation_history_ready_ = true;
  }

  // The in-tune tongue, and the musette partners (gated): phase accumulators, averaged.
  phase_a_ += inc_a_ * ratio;
  phase_a_ -= std::floor(phase_a_);
  for (int i = 0; i < partner_count_; ++i) {
    Partner& t = partners_[i];
    t.phase += t.inc * ratio;
    t.phase -= std::floor(t.phase);
  }

  float tongue;
  if (slot_duty_ > 0.0f) {
    // The first difference has magnitude 2*sin(pi*f/sr), where f follows the
    // live pitch ratio. Re-cut its normalizer only when radiation is active
    // and the ratio moved; ratio 1 retains start()'s original operation.
    if (radiation_ > 0.0f && ratio != radiation_ratio_) {
      radiation_norm_ =
          radiation_norm(radiation_halfangle_, ratio, static_cast<float>(sample_rate_));
      radiation_mix_norm_ = radiation_mix_norm(radiation_, radiation_halfangle_, ratio,
                                               static_cast<float>(sample_rate_));
      for (int i = 0; i < partner_count_; ++i) {
        Partner& t = partners_[i];
        t.norm = radiation_norm(t.halfangle, ratio, static_cast<float>(sample_rate_));
        t.mix_norm =
            radiation_mix_norm(radiation_, t.halfangle, ratio, static_cast<float>(sample_rate_));
      }
      radiation_ratio_ = ratio;
    }
    // Slot flow (gated): the tongue swings through the slot and lets air past
    // twice per cycle, so the source is a pair of humps rather than a shaped
    // saw. Radiating it is what lifts the second partial over the first.
    float flow = slot_flow_at(phase_a_);
    if (partner_count_ > 0) {
      if (radiation_ > 0.0f) {
        flow -= slot_mean_;
        const float radiated = (flow - prev_flow_) * radiation_norm_;
        prev_flow_ = flow;
        float sum = radiation_ >= 1.0f
                        ? flow + radiation_ * (radiated - flow)
                        : (flow + radiation_ * (radiated - flow)) * radiation_mix_norm_;
        for (int i = 0; i < partner_count_; ++i) {
          Partner& t = partners_[i];
          const float flow_t = slot_flow_at(t.phase) - slot_mean_;
          const float radiated_t = (flow_t - t.prev_flow) * t.norm;
          t.prev_flow = flow_t;
          sum += partner_level_ *
                 (radiation_ >= 1.0f ? flow_t + radiation_ * (radiated_t - flow_t)
                                     : (flow_t + radiation_ * (radiated_t - flow_t)) * t.mix_norm);
        }
        tongue = tongue_mix_ * sum;
      } else {
        // The dry path averages the tongues' flows before taking off the steady mean.
        float sum = flow;
        for (int i = 0; i < partner_count_; ++i) {
          sum += partner_level_ * slot_flow_at(partners_[i].phase);
        }
        flow = tongue_mix_ * sum;
        flow -= slot_mean_;
        tongue = flow;
      }
    } else {
      flow -= slot_mean_;
      const float radiated = (flow - prev_flow_) * radiation_norm_;
      prev_flow_ = flow;
      tongue = radiation_ >= 1.0f ? flow + radiation_ * (radiated - flow)
                                  : (flow + radiation_ * (radiated - flow)) * radiation_mix_norm_;
    }
  } else {
    // The soft-clipped asymmetric saw: the in-slot and out-of-slot half-cycles
    // get different gains, giving a skewed even-and-odd harmonic buzz.
    const float saw_a = 2.0f * phase_a_ - 1.0f;
    const float gain_a = saw_a >= 0.0f ? (1.0f + asymmetry_) : (1.0f - asymmetry_);
    tongue = std::tanh(drive_ * gain_a * saw_a);
    if (partner_count_ > 0) {
      float sum = tongue;
      for (int i = 0; i < partner_count_; ++i) {
        const float saw_t = 2.0f * partners_[i].phase - 1.0f;
        const float gain_t = saw_t >= 0.0f ? (1.0f + asymmetry_) : (1.0f - asymmetry_);
        sum += partner_level_ * std::tanh(drive_ * gain_t * saw_t);
      }
      tongue = tongue_mix_ * sum;
    }
  }

  // Leakage air hiss around the reed, before the body filter so it shares the
  // radiation colour.
  if (breath_noise_ > 0.0f) {
    tongue += breath_noise_ * noise_.bipolar_at(drive_index_);
  }
  ++drive_index_;

  // Reed-plate / cavity radiation: one-pole roll-off.
  body_state_ += body_alpha_ * (tongue - body_state_);

  return body_state_ * bellows_.level * output_scale_;
}

float FreeReedVoiceCore::slot_flow_at(float phase) const noexcept {
  return swing_ > 0.0f ? swing_flow(phase, swing_)
                       : slot_flow(phase, slot_duty_, slot_return_, slot_gap_, slot_return_width_);
}

void FreeReedVoiceCore::release() noexcept { bellows_.release(); }

void FreeReedVoiceCore::kill() noexcept {
  bellows_.kill();
  body_state_ = 0.0f;
  phase_a_ = 0.0f;
  for (Partner& t : partners_) {
    t.phase = 0.0f;
    t.prev_flow = 0.0f;
  }
  prev_flow_ = 0.0f;
  radiation_history_ready_ = false;
}

}  // namespace sonare::midi::synth
