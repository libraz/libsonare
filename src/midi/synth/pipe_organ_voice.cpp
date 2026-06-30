#include "midi/synth/pipe_organ_voice.h"

#include <algorithm>
#include <cmath>

#include "rt/fractional_delay.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Onset / drive calibration (tuned so a held note speaks promptly, sustains at
// a steady level, and never overflows; see pipe_organ_voice_test).
/// Pre-fill amplitude that seeds the loop for prompt speech.
constexpr float kFillAmp = 0.55f;
/// Maps the breath param to the steady drive that replaces the loop loss.
constexpr float kBreathGain = 4.0f;
/// Maps the chiff param to the onset burst level.
constexpr float kChiffGain = 0.9f;

/// Noise draw index bases (kept far apart so the pre-fill, breath and chiff
/// streams never reuse the same draws on a single per-voice seed).
constexpr uint64_t kFillIndexBase = 1ull << 16;
constexpr uint64_t kBreathIndexBase = 1ull << 20;
constexpr uint64_t kChiffIndexBase = 1ull << 24;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s.
float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  return std::exp(-6.907755279f / loops_to_t60);
}

}  // namespace

void PipeOrganVoiceCore::start(const PipeOrganPatchParams& params, double sample_rate, uint8_t note,
                               uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;

  const float f0 = note_to_hz(note);
  const float period = static_cast<float>(sr) / f0;
  // Open pipe: a positive-feedback comb of one full period (all harmonics).
  // Stopped pipe: a negative-feedback comb of half the length, so the impulse
  // response flips sign every base_period_ samples (period 2*base_period_ ==
  // one fundamental period) and only the odd harmonics survive.
  base_period_ = params.stopped ? 0.5f * period : period;
  loop_sign_ = params.stopped ? -1.0f : 1.0f;

  // Loop lowpass: brightness -> feedback pole a (y += (1-a)(x-y)).
  const float a = (1.0f - std::clamp(params.brightness, 0.0f, 1.0f)) * 0.7f;
  loop_alpha_ = 1.0f - a;
  lp_state_ = 0.0f;
  // In-loop DC blocker pole (~8 Hz): kills the open comb's DC pressure mode.
  dc_x1_ = 0.0f;
  dc_y1_ = 0.0f;
  dc_r_ = 1.0f - static_cast<float>(kTwoPi * 8.0 / sr);
  // Tuning: compensate the loop filter's exact phase delay at the FUNDAMENTAL
  // (f0 either way) plus the one-sample feedback path, so the sounding pitch
  // matches the note for both the open and the (half-length) stopped comb.
  const float omega = kTwoPi / period;
  const float tau_lp =
      std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
  // The in-loop DC blocker (set below) leads in phase near the fundamental,
  // shortening the effective loop and sounding the pipe sharp in the bass.
  // Compensate its exact phase delay at f0 too: H_dc = (1 - z^-1)/(1 - r z^-1)
  // (dc_r_ is set in the loop-filter block above).
  const float phase_dc = std::atan2(std::sin(omega), 1.0f - std::cos(omega)) -
                         std::atan2(dc_r_ * std::sin(omega), 1.0f - dc_r_ * std::cos(omega));
  const float tau_dc = phase_dc / std::max(omega, 1.0e-6f);
  loop_comp_ = 1.0f + tau_lp - tau_dc;

  // Resonator Q: t60 sets the undriven ring (no keyboard stretch — a pipe is
  // sustained by wind, not a decaying string). period_samples is base_period_,
  // so the stopped comb's two traversals per cycle stay consistent.
  const float t60 = std::max(0.05f, params.tone_decay_s);
  loop_gain_ = loop_gain_for(base_period_, sr, t60);
  release_gain_ = loop_gain_for(base_period_, sr, std::max(0.01f, params.release_damp_s));

  // Velocity gives a gentle touch on level only (an organ key is on/off).
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float fill_amp = kFillAmp * (0.7f + 0.3f * vel01);

  // Steady jet drive that holds the tone. A white-noise-driven resonator of
  // pole radius g settles to an amplitude ~ drive / sqrt(1 - g^2), so scaling
  // the drive by sqrt(1 - g^2) pins the SUSTAINED level independent of the ring
  // time: the breath replaces exactly the energy the loop bleeds, and the pipe
  // neither swells nor decays while the wind is on.
  const float loss = std::sqrt(std::max(0.0f, 1.0f - loop_gain_ * loop_gain_));
  breath_level_ = std::clamp(params.breath, 0.0f, 1.0f) * loss * fill_amp * kBreathGain;
  // High-pass the breath just below the fundamental (0.6*f0) so it energises
  // the harmonics, not the DC comb mode (open pipe) or a sub-audio wander —
  // note-relative so it never eats the fundamental of a low pipe.
  breath_hp_state_ = 0.0f;
  breath_hp_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * 0.6f * f0 / static_cast<float>(sr)), 0.0f, 1.0f);

  // Chiff: a bright onset burst added post-loop, decaying through a one-pole.
  chiff_level_ = std::clamp(params.chiff, 0.0f, 1.0f) * fill_amp * kChiffGain;
  chiff_coeff_ = std::exp(
      -1.0f / std::max(1.0f, static_cast<float>(std::max(0.5f, params.chiff_ms) * 0.001 * sr)));

  // Circular span for this note: the loop period plus bend-down headroom
  // (+2 semitones ~= x1.13) and the interpolator's stencil margin.
  size_ = std::min(capacity_, static_cast<int>(base_period_ * 1.3f) + 8);
  write_index_ = 0;
  // Pre-fill the loop with the seeded onset burst so the pipe speaks at full
  // amplitude immediately (the Karplus-Strong trick) instead of swelling in.
  if (buffer_ != nullptr) {
    for (int i = 0; i < size_; ++i) {
      buffer_[static_cast<size_t>(i)] =
          fill_amp * noise_.bipolar_at(kFillIndexBase + static_cast<uint64_t>(i));
    }
  }
}

float PipeOrganVoiceCore::render(float pitch_ratio) noexcept {
  if (buffer_ == nullptr || size_ < 8) return 0.0f;

  // Chiff: decaying bright onset noise on top of the pitched tone.
  float chiff = 0.0f;
  if (chiff_level_ > 1.0e-5f) {
    chiff = chiff_level_ * noise_.bipolar_at(kChiffIndexBase + drive_index_);
    chiff_level_ *= chiff_coeff_;
  }

  // Steady jet turbulence replacing the loop loss, high-passed so the open
  // pipe's DC comb mode is not pumped (drive = noise - lowpass(noise)).
  const float raw = noise_.bipolar_at(kBreathIndexBase + drive_index_);
  breath_hp_state_ += breath_hp_alpha_ * (raw - breath_hp_state_);
  const float drive = breath_level_ * (raw - breath_hp_state_);
  ++drive_index_;

  // pitch_ratio scales the frequency, so it divides the loop delay.
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  const float delay =
      std::clamp(base_period_ / ratio - loop_comp_, 1.0f, static_cast<float>(size_ - 4));
  const int delay_q8 = static_cast<int>(delay * 256.0f);

  const float fb = loop_sign_ * loop_gain_ * lp_state_;
  // DC-block the signal entering the delay line so the open comb's DC mode
  // cannot charge up (radiation high-pass).
  const float in = drive + fb;
  const float in_dc = in - dc_x1_ + dc_r_ * dc_y1_;
  dc_x1_ = in;
  dc_y1_ = in_dc;
  const float out = rt::lagrange3_fractional_delay(buffer_, static_cast<size_t>(size_),
                                                   write_index_, delay_q8, in_dc);
  lp_state_ += loop_alpha_ * (out - lp_state_);
  return out + chiff;
}

void PipeOrganVoiceCore::release() noexcept {
  // Wind off: stop driving the jet and damp the column to a short tail.
  loop_gain_ = std::min(loop_gain_, release_gain_);
  breath_level_ = 0.0f;
  chiff_level_ = 0.0f;
}

void PipeOrganVoiceCore::kill() noexcept {
  loop_gain_ = 0.0f;
  lp_state_ = 0.0f;
  breath_level_ = 0.0f;
  chiff_level_ = 0.0f;
}

}  // namespace sonare::midi::synth
