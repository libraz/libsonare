#include "midi/synth/plucked_string_voice.h"

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

using sonare::constants::kTwoPi;

/// Excitation noise draws live far above the voice-level draw indices
/// (detune/phase/drift use 0..~103 on the same per-voice seed).
constexpr uint64_t kNoiseIndexBase = 1ull << 16;

/// Bridge-limiter travel above the threshold: the curved surface gives a little
/// before the string is fully pinned, and a stronger jawari sits the string
/// closer to the surface (less give -> harder knee -> more high partials).
SONARE_TUNABLE(kBuzzSpanBase, 0.35f);

/// Output trim bringing the raw string loop up to a musical voice level (a
/// forte pluck lands roughly in [0.3, 0.8] peak).
SONARE_TUNABLE(kPluckedOutputScale, 0.85f);

/// Second point past which the loop-loss pole's darkening is quoted (Hz), so a
/// fixed brightness value means the same tilt at every sample rate. Argued from
/// the family's register (sitar/koto/shamisen run guitar-like, well under this)
/// rather than measured.
SONARE_TUNABLE(kPluckedBrightnessRefHz, 2500.0f);

/// The t60-ratio fraction that reproduces the shipped pole's own darkening at
/// kPluckedBrightnessRefHz, relative to the fundamental's OWN t60 -- read at a
/// fixed anchor (note 60, 48 kHz, this patch's own decay_s/decay_stretch)
/// rather than at the note actually playing. t60_ref = t60 * hf: same shape as
/// ks_voice.cpp's quote_t60, harpsichord_voice.cpp's hf and
/// piano_voice.cpp's t60_slow/ratio -- the reference partial rings for a fixed
/// FRACTION of the fundamental's own t60, whatever note is played, rather than
/// losing a fixed amount of gain per traversal regardless of how long the
/// fundamental itself is meant to ring.
float brightness_hf(float brightness, float decay_s, float decay_stretch) noexcept {
  const float a_ship = (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * 0.7f;
  constexpr float kAnchorSr = 48000.0f;
  constexpr uint8_t kAnchorNote = 60;
  const float anchor_period = kAnchorSr / note_to_hz(static_cast<float>(kAnchorNote));
  const float w0 = kTwoPi / anchor_period;
  const float w_ref = kTwoPi * kPluckedBrightnessRefHz / kAnchorSr;
  const float tilt_a = onepole_magnitude(a_ship, w_ref) / onepole_magnitude(a_ship, w0);

  const float stretch = std::clamp(decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(kAnchorNote)) / 12.0f;
  const float t60_a = std::max(0.05f, decay_s) * std::exp2(stretch * octaves_below_a4);
  const float g0_a = string_loop_gain_for(anchor_period, kAnchorSr, t60_a);
  return 1.0f / (1.0f + std::log(tilt_a) / std::log(g0_a));
}

}  // namespace

void PluckedStringVoiceCore::start(const PluckedStringPatchParams& params, double sample_rate,
                                   uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);

  const float f0 = note_to_hz(note);
  base_period_ = static_cast<float>(sr) / f0;

  // Decay: t60 stretched per octave below A4 (low strings ring longer). This
  // target is already correct in Hz/seconds; only the loop-lowpass pole below
  // was defective (fixed in samples, so the same brightness rendered a
  // different instrument at a different sample rate).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float t60 = std::max(0.05f, params.decay_s) * std::exp2(stretch * octaves_below_a4);

  // Loop loss: solve the pole from the fundamental's own decay target and the
  // brightness-set tilt at a reference frequency in Hz, rather than reading
  // brightness straight into a pole fixed in samples.
  const float omega = kTwoPi / base_period_;
  const float omega_ref = kTwoPi * kPluckedBrightnessRefHz / static_cast<float>(sr);
  const float hf = brightness_hf(params.brightness, params.decay_s, params.decay_stretch);
  const float g0 = string_loop_gain_for(base_period_, sr, t60);
  const float g_ref = string_loop_gain_for(base_period_, sr, t60 * hf);
  const StringLoopFilter loss = solve_string_loop_filter(omega, omega_ref, g0, g_ref);
  loop_alpha_ = 1.0f - loss.a;
  loop_gain_ = loss.g;
  lp_state_ = 0.0f;
  // Tuning: compensate the EXACT phase delay of the loop filter at the
  // fundamental (not just its DC group delay) plus the one-sample feedback
  // path, so the sounding pitch matches the note to a few cents.
  const float tau_lp = onepole_group_delay_samples(loss.a, omega);
  loop_comp_ = 1.0f + tau_lp;

  release_gain_ = string_loop_gain_for(base_period_, sr, std::max(0.01f, params.release_damp_s));

  // Buzzing bridge: a stronger jawari sits the threshold lower into the
  // string's swing, so the returning wave grazes the curved surface on more of
  // every period. 0 disables the limiter entirely so the render path stays
  // bit-identical to the plain plucked string.
  buzz_amount_ = std::clamp(params.buzz, 0.0f, 1.0f);
  buzz_threshold_ = buzz_amount_ > 0.0f ? 0.6f - 0.4f * buzz_amount_ : 0.0f;

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
  // Exponential brightness -> cutoff map (300 Hz .. ~12 kHz) through a single
  // one-pole, so the velocity swing is clearly audible.
  const float exc_cutoff = 300.0f * std::exp2(5.3f * bright);
  exc_alpha_ =
      std::clamp(1.0f - std::exp(-kTwoPi * exc_cutoff / static_cast<float>(sr)), 0.01f, 1.0f);
  exc_lp_ = 0.0f;

  output_scale_ = kPluckedOutputScale;

  // The line spans the whole slab rather than this note's period. The line
  // length is what bounds a downward bend, and the clamp that enforces it
  // saturates silently, so a span cut to the note-on period is a pitch ceiling
  // with nothing to hear it by.
  write_index_ = 0;
  if (buffer_ != nullptr) {
    std::fill(buffer_, buffer_ + static_cast<size_t>(std::max(0, capacity_)), 0.0f);
  }
}

float PluckedStringVoiceCore::render(float pitch_ratio) noexcept {
  if (buffer_ == nullptr || capacity_ < 8) return 0.0f;

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
    exc_lp_ += exc_alpha_ * (burst - exc_lp_);
    exc = 0.7f * exc_lp_;  // comb headroom
  }

  // pitch_ratio scales the frequency, so it divides the loop delay.
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  const float delay =
      std::clamp(base_period_ / ratio - loop_comp_, 1.0f, static_cast<float>(capacity_ - 4));
  const int delay_q8 = static_cast<int>(delay * 256.0f);

  float loop_in = exc + loop_gain_ * lp_state_;
  if (buzz_threshold_ > 0.0f && loop_in > buzz_threshold_) {
    // Buzzing bridge: displacement toward the curved surface is softly limited
    // once it grazes the threshold, so the flattened crest sprays energy into
    // the high partials each period (one-sided, memoryless). The rational
    // saturator keeps the crest strictly below threshold + span, so the loop
    // stays bounded; a stronger jawari leaves less give above the surface.
    const float over = loop_in - buzz_threshold_;
    const float span = kBuzzSpanBase * (1.0f - 0.5f * buzz_amount_);
    loop_in = buzz_threshold_ + span * over / (span + over);
  }

  const float out = rt::lagrange3_fractional_delay(buffer_, static_cast<size_t>(capacity_),
                                                   write_index_, delay_q8, loop_in);
  lp_state_ += loop_alpha_ * (out - lp_state_);
  return out * output_scale_;
}

void PluckedStringVoiceCore::release() noexcept {
  loop_gain_ = std::min(loop_gain_, release_gain_);
}

void PluckedStringVoiceCore::kill() noexcept {
  exc_pos_ = exc_total_ + pick_delay_;
  loop_gain_ = 0.0f;
  lp_state_ = 0.0f;
  exc_lp_ = 0.0f;
}

}  // namespace sonare::midi::synth
