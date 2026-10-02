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

float radiation_norm(float halfangle, float ratio, float sample_rate) noexcept {
  // A first difference has no useful fundamental calibration below 20 Hz or
  // above Nyquist. Keeping the effective angle in that physical interval also
  // prevents an aliased pitch from landing on a numerical zero of sin().
  const float nyquist_halfangle = 0.25f * static_cast<float>(kTwoPi);
  const float minimum_halfangle =
      std::min(0.5f * static_cast<float>(kTwoPi) * 20.0f / sample_rate, nyquist_halfangle);
  const float effective_halfangle =
      std::clamp(std::abs(halfangle * ratio), minimum_halfangle, nyquist_halfangle);
  return 1.0f / std::max(2.0f * std::abs(std::sin(effective_halfangle)), 1.0e-6f);
}

}  // namespace

void FreeReedVoiceCore::start(const FreeReedPatchParams& params, double sample_rate, uint8_t note,
                              Velocity16 velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  sample_rate_ = sr;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 1;
  releasing_ = false;
  level_ = 0.0f;
  level_target_ = 1.0f;
  body_state_ = 0.0f;

  base_freq_hz_ = note_to_hz(note);
  // Base phase increments (cycles per sample); render() scales them by the live
  // pitch ratio so bend/drift keep the musette pair's ratio intact.
  inc_a_ = static_cast<float>(base_freq_hz_ / sr);

  // Musette pair: a second tongue a few cents sharp. detune == 0 keeps dual_
  // false so the single-tongue path never touches the second oscillator.
  const float detune = std::clamp(params.detune, 0.0f, 1.0f);
  dual_ = detune > 0.0f;
  phase_a_ = 0.0f;
  if (dual_) {
    const float cents = kDetuneMinCents + kDetuneSpanCents * detune;
    inc_b_ = inc_a_ * cents_to_ratio(cents);
    // Decorrelate the pair's start (a real accordion's tongues never speak in
    // phase); seeded, so the offset is deterministic per voice.
    phase_b_ = noise_.unipolar_at(0);
  } else {
    inc_b_ = 0.0f;
    phase_b_ = 0.0f;
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
  // A first difference is the monopole's +6 dB/octave, and it is 2*sin(pi*f0/sr)
  // at the fundamental — small, and note-dependent. Normalising it away leaves
  // the tilt without the level or the register slope that come with it.
  radiation_halfangle_ = static_cast<float>(kTwoPi * base_freq_hz_ / (2.0 * sr));
  radiation_halfangle_b_ = dual_ ? 0.5f * static_cast<float>(kTwoPi) * inc_b_ : 0.0f;
  radiation_ratio_ = 1.0f;
  radiation_norm_ = radiation_norm(radiation_halfangle_, 1.0f, static_cast<float>(sr));
  radiation_norm_b_ =
      dual_ ? radiation_norm(radiation_halfangle_b_, 1.0f, static_cast<float>(sr)) : 0.0f;

  // The differentiator history is primed on the first render, after its live
  // pitch ratio is known. Keep the old zero history for a normal mono start.
  prev_flow_ = 0.0f;
  prev_flow_b_ = 0.0f;
  radiation_history_ready_ = false;

  // Contour + textures.
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);
  // A per-sample draw voiced at kLossVoicedSr, so it carries the noise law's gain.
  breath_noise_ =
      std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathNoiseDepth * noise_gain_at_rate(sr);
  // The radiated pair swings wider than the saturated saw it replaces, so the
  // patch gains stay comparable across the two shapers.
  slot_makeup_ = slot_duty_ > 0.0f ? kSlotMakeup : 1.0f;
  output_scale_ = (kOutputMin + kOutputSpan * level) * slot_makeup_;

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
  output_scale_ = (kOutputMin + kOutputSpan * force01_) * slot_makeup_;
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
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  level_ += coeff * (level_target_ - level_);

  if (!radiation_history_ready_) {
    const float effective_frequency = base_freq_hz_ * ratio;
    const float nyquist = 0.5f * static_cast<float>(sample_rate_);
    if (slot_duty_ > 0.0f &&
        (dual_ || effective_frequency < 20.0f || effective_frequency > nyquist)) {
      // This is deliberately before phase advance: the first difference must
      // compare the first rendered phase against the phase that preceded it.
      prev_flow_ =
          slot_flow(phase_a_, slot_duty_, slot_return_, slot_gap_, slot_return_width_) - slot_mean_;
      if (dual_) {
        prev_flow_b_ =
            slot_flow(phase_b_, slot_duty_, slot_return_, slot_gap_, slot_return_width_) -
            slot_mean_;
      }
    }
    radiation_history_ready_ = true;
  }

  // Tongue A, and the musette partner (gated): two phase accumulators, averaged.
  phase_a_ += inc_a_ * ratio;
  phase_a_ -= std::floor(phase_a_);
  if (dual_) {
    phase_b_ += inc_b_ * ratio;
    phase_b_ -= std::floor(phase_b_);
  }

  float tongue;
  if (slot_duty_ > 0.0f) {
    // The first difference has magnitude 2*sin(pi*f/sr), where f follows the
    // live pitch ratio. Re-cut its normalizer only when radiation is active
    // and the ratio moved; ratio 1 retains start()'s original operation.
    if (radiation_ > 0.0f && ratio != radiation_ratio_) {
      radiation_norm_ =
          radiation_norm(radiation_halfangle_, ratio, static_cast<float>(sample_rate_));
      if (dual_) {
        radiation_norm_b_ =
            radiation_norm(radiation_halfangle_b_, ratio, static_cast<float>(sample_rate_));
      }
      radiation_ratio_ = ratio;
    }
    // Slot flow (gated): the tongue swings through the slot and lets air past
    // twice per cycle, so the source is a pair of humps rather than a shaped
    // saw. Radiating it is what lifts the second partial over the first.
    float flow = slot_flow(phase_a_, slot_duty_, slot_return_, slot_gap_, slot_return_width_);
    if (dual_) {
      const float flow_b =
          slot_flow(phase_b_, slot_duty_, slot_return_, slot_gap_, slot_return_width_);
      if (radiation_ > 0.0f) {
        flow -= slot_mean_;
        const float radiated = (flow - prev_flow_) * radiation_norm_;
        prev_flow_ = flow;
        const float tongue_a = flow + radiation_ * (radiated - flow);

        float flow_b_centered = flow_b - slot_mean_;
        const float radiated_b = (flow_b_centered - prev_flow_b_) * radiation_norm_b_;
        prev_flow_b_ = flow_b_centered;
        const float tongue_b = flow_b_centered + radiation_ * (radiated_b - flow_b_centered);
        tongue = 0.5f * (tongue_a + tongue_b);
      } else {
        // Preserve the established dual dry path's average-then-subtract
        // operation exactly when no radiation is requested.
        flow = 0.5f * (flow + flow_b);
        flow -= slot_mean_;
        tongue = flow;
      }
    } else {
      flow -= slot_mean_;
      const float radiated = (flow - prev_flow_) * radiation_norm_;
      prev_flow_ = flow;
      tongue = flow + radiation_ * (radiated - flow);
    }
  } else {
    // The soft-clipped asymmetric saw: the in-slot and out-of-slot half-cycles
    // get different gains, giving a skewed even-and-odd harmonic buzz.
    const float saw_a = 2.0f * phase_a_ - 1.0f;
    const float gain_a = saw_a >= 0.0f ? (1.0f + asymmetry_) : (1.0f - asymmetry_);
    tongue = std::tanh(drive_ * gain_a * saw_a);
    if (dual_) {
      const float saw_b = 2.0f * phase_b_ - 1.0f;
      const float gain_b = saw_b >= 0.0f ? (1.0f + asymmetry_) : (1.0f - asymmetry_);
      tongue = 0.5f * (tongue + std::tanh(drive_ * gain_b * saw_b));
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

  return body_state_ * level_ * output_scale_;
}

void FreeReedVoiceCore::release() noexcept {
  releasing_ = true;
  level_target_ = 0.0f;
}

void FreeReedVoiceCore::kill() noexcept {
  level_ = 0.0f;
  level_target_ = 0.0f;
  body_state_ = 0.0f;
  phase_a_ = 0.0f;
  phase_b_ = 0.0f;
  prev_flow_ = 0.0f;
  prev_flow_b_ = 0.0f;
  radiation_history_ready_ = false;
  releasing_ = true;
}

}  // namespace sonare::midi::synth
