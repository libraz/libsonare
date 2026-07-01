#include "midi/synth/ks_voice.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
/// KS noise draws live far above the voice-level draw indices (detune/phase/
/// drift use 0..~103 on the same per-voice seed).
constexpr uint64_t kNoiseIndexBase = 1ull << 16;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s.
float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  // 0.001^(1/loops): -60 dB spread across the loops within t60.
  return std::exp(-6.907755279f / loops_to_t60);
}

}  // namespace

void KsVoiceCore::start(const KsPatchParams& params, double sample_rate, uint8_t note,
                        uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);

  const float f0 = note_to_hz(note);
  base_period_ = static_cast<float>(sr) / f0;

  // Loop lowpass: brightness -> feedback coefficient a (y += (1-a)(x-y)).
  const float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f;
  loop_alpha_ = 1.0f - a;
  lp_state_ = 0.0f;
  // Tuning: compensate the EXACT phase delay of the loop filter at the
  // fundamental (not just its DC group delay) plus the one-sample feedback
  // path, so the sounding pitch matches the note to a few cents.
  const float omega = kTwoPi / base_period_;
  const float tau_lp =
      std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
  loop_comp_ = 1.0f + tau_lp;

  // Decay: t60 stretched per octave below A4 (low strings ring longer).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);
  loop_gain_ = loop_gain_for(base_period_, sr, t60);
  release_gain_ = loop_gain_for(base_period_, sr, std::max(0.01f, params.release_damp_s));

  // Fret-slap: a narrower displacement gap for higher intensity. 0 disables the
  // limiter entirely so the render path stays bit-identical to the plain string.
  const float slap = std::clamp(params.slap, 0.0f, 1.0f);
  slap_threshold_ = slap > 0.0f ? 0.55f - 0.35f * slap : 0.0f;

  // Excitation: one period of seeded noise through the pick-position comb and
  // the velocity-driven dynamic-level lowpass (hard pluck = bright).
  exc_total_ = std::max(8, static_cast<int>(base_period_));
  exc_pos_ = 0;
  pick_delay_ =
      static_cast<int>(std::clamp(params.pick_position, 0.0f, 0.5f) * base_period_ + 0.5f);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float bright =
      std::clamp(params.exc_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);
  // Exponential brightness -> cutoff map (300 Hz .. ~12 kHz) through two
  // cascaded one-poles, so the velocity swing is clearly audible.
  const float exc_cutoff = 300.0f * std::exp2(5.3f * bright);
  exc_alpha_ = std::clamp(1.0f - std::exp(-6.28318530718f * exc_cutoff / static_cast<float>(sr)),
                          0.01f, 1.0f);
  exc_lp1_ = 0.0f;
  exc_lp2_ = 0.0f;

  // Circular span for this note: the base period plus bend-down headroom
  // (+2 semitones ~= x1.13) and the interpolator's stencil margin.
  size_ = std::min(capacity_, static_cast<int>(base_period_ * 1.3f) + 8);
  write_index_ = 0;
  if (buffer_ != nullptr) {
    std::fill(buffer_, buffer_ + static_cast<size_t>(std::max(0, size_)), 0.0f);
  }

  // Second (horizontal) polarization (off unless params.polarization > 0 ->
  // render skips it, primary path bit-identical). A second loop detuned a few
  // cents sharp, more damped and decaying faster than the primary: the two
  // planes beat and the faster line dies first (two-stage decay).
  const float polarization = std::clamp(params.polarization, 0.0f, 1.0f);
  pol_write_ = 0;
  pol_lp_state_ = 0.0f;
  if (polarization > 0.0f && pol_buffer_ != nullptr) {
    constexpr float kPolDetuneCents = 11.0f;
    pol_period_ = base_period_ / std::exp2(kPolDetuneCents / 1200.0f);
    // A darker loop filter: the horizontal plane loses its highs faster.
    const float a2 = std::min(0.97f, a + 0.12f);
    pol_loop_alpha_ = 1.0f - a2;
    const float omega2 = kTwoPi / pol_period_;
    const float tau2 =
        std::atan2(a2 * std::sin(omega2), 1.0f - a2 * std::cos(omega2)) / std::max(omega2, 1.0e-6f);
    pol_loop_comp_ = 1.0f + tau2;
    // Faster decay (a fraction of the primary t60) -> two-stage decay.
    pol_loop_gain_ = loop_gain_for(pol_period_, sr, 0.55f * t60);
    pol_release_gain_ = release_gain_;
    pol_exc_ = 0.6f;  // the pluck grips the vertical plane; the horizontal weakly
    pol_couple_ = polarization;
    pol_size_ = std::min(capacity_, static_cast<int>(pol_period_ * 1.3f) + 8);
    std::fill(pol_buffer_, pol_buffer_ + static_cast<size_t>(std::max(0, pol_size_)), 0.0f);
  } else {
    pol_couple_ = 0.0f;
    pol_loop_gain_ = 0.0f;
  }
}

float KsVoiceCore::render(float pitch_ratio) noexcept {
  if (buffer_ == nullptr || size_ < 8) return 0.0f;

  float exc = 0.0f;
  if (exc_pos_ < exc_total_ + pick_delay_) {
    // Pick-position comb: burst[n] - burst[n - pick_delay]. The delayed copy
    // runs pick_delay samples past the burst so the comb notches stay exact.
    float burst = exc_pos_ < exc_total_
                      ? noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(exc_pos_))
                      : 0.0f;
    if (pick_delay_ > 0 && exc_pos_ >= pick_delay_) {
      burst -= noise_.bipolar_at(kNoiseIndexBase + static_cast<uint64_t>(exc_pos_ - pick_delay_));
    }
    ++exc_pos_;
    exc_lp1_ += exc_alpha_ * (burst - exc_lp1_);
    exc_lp2_ += exc_alpha_ * (exc_lp1_ - exc_lp2_);
    exc = 0.7f * exc_lp2_;  // comb headroom
  }

  // pitch_ratio scales the frequency, so it divides the loop delay.
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  const float delay =
      std::clamp(base_period_ / ratio - loop_comp_, 1.0f, static_cast<float>(size_ - 4));
  const int delay_q8 = static_cast<int>(delay * 256.0f);

  const float fb = loop_gain_ * lp_state_;
  float loop_in = exc + fb;
  if (slap_threshold_ > 0.0f) {
    // Fret contact: the string cannot swing past the fret gap. Over-travel is
    // hard-limited with only a sliver of give, so the clipped tops generate the
    // odd-harmonic buzz of the slap/pop attack (a memoryless nonlinear limit).
    constexpr float kReflect = 0.06f;  // near-hard clip at the fret gap
    const float th = slap_threshold_;
    if (loop_in > th) {
      loop_in = th + (loop_in - th) * kReflect;
    } else if (loop_in < -th) {
      loop_in = -th + (loop_in + th) * kReflect;
    }
  }
  const float out = rt::lagrange3_fractional_delay(buffer_, static_cast<size_t>(size_),
                                                   write_index_, delay_q8, loop_in);
  lp_state_ += loop_alpha_ * (out - lp_state_);

  if (pol_couple_ > 0.0f) {
    // Horizontal polarization: a detuned loop sharing the pluck; its output
    // sums into the mix, beating against the primary (two-stage decay).
    const float pol_delay =
        std::clamp(pol_period_ / ratio - pol_loop_comp_, 1.0f, static_cast<float>(pol_size_ - 4));
    const int pol_delay_q8 = static_cast<int>(pol_delay * 256.0f);
    const float pol_in = pol_exc_ * exc + pol_loop_gain_ * pol_lp_state_;
    const float pol_out = rt::lagrange3_fractional_delay(
        pol_buffer_, static_cast<size_t>(pol_size_), pol_write_, pol_delay_q8, pol_in);
    pol_lp_state_ += pol_loop_alpha_ * (pol_out - pol_lp_state_);
    return out + pol_couple_ * pol_out;
  }
  return out;
}

void KsVoiceCore::release() noexcept {
  loop_gain_ = std::min(loop_gain_, release_gain_);
  if (pol_couple_ > 0.0f) pol_loop_gain_ = std::min(pol_loop_gain_, pol_release_gain_);
}

void KsVoiceCore::kill() noexcept {
  exc_pos_ = exc_total_;
  loop_gain_ = 0.0f;
  lp_state_ = 0.0f;
  pol_loop_gain_ = 0.0f;
  pol_lp_state_ = 0.0f;
}

}  // namespace sonare::midi::synth
