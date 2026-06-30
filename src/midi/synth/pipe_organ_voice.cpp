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
/// Reed valve drive inside the loop saturator: the small-signal gain that lets
/// the reed self-oscillate into a bright, harmonic-rich limit cycle.
constexpr float kReedDrive = 2.6f;

/// Noise draw index bases (kept far apart so the pre-fill, breath and chiff
/// streams never reuse the same draws on a single per-voice seed). A per-rank
/// offset in the high bits (kRankNoiseShift) separates the ranks on top.
constexpr uint64_t kFillIndexBase = 1ull << 16;
constexpr uint64_t kBreathIndexBase = 1ull << 20;
constexpr uint64_t kChiffIndexBase = 1ull << 24;
constexpr uint64_t kRankNoiseShift = 48;

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

  // Resolve the registration: an explicit rank list, or a single implicit 8'
  // rank built from the flat {stopped, brightness} (keeps the single-pipe
  // presets — church-flute / church-bourdon — behaving exactly as before).
  PipeOrganRank implicit{};
  implicit.footage_mult = 1.0f;
  implicit.stopped = params.stopped;
  implicit.brightness = params.brightness;
  implicit.level = 1.0f;
  implicit.reed = params.reed;
  const PipeOrganRank* ranks = &implicit;
  int count = 1;
  if (params.rank_count > 0) {
    ranks = params.ranks.data();
    count = std::min(params.rank_count, kMaxPipeRanks);
  }
  rank_count_ = count;

  // Chorus normalisation: sum of decorrelated pipes adds in power, so divide by
  // sqrt(sum level^2) to hold the stop at a single-pipe loudness (the fullness
  // comes from the spectrum, not raw level; the patch gain sets the level).
  float power = 0.0f;
  for (int r = 0; r < count; ++r) {
    const float lvl = std::clamp(ranks[r].level, 0.0f, 1.0f);
    power += lvl * lvl;
  }
  const float norm = 1.0f / std::sqrt(std::max(1.0e-6f, power));

  const float base_f0 = note_to_hz(note);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float fill_amp = kFillAmp * (0.7f + 0.3f * vel01);
  const float t60 = std::max(0.05f, params.tone_decay_s);
  const float rel_t60 = std::max(0.01f, params.release_damp_s);

  for (int r = 0; r < count; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.buffer = slab_ != nullptr ? slab_ + static_cast<size_t>(r) * rank_capacity_ : nullptr;
    pipe.mix = std::clamp(ranks[r].level, 0.0f, 1.0f) * norm;
    pipe.noise_offset = static_cast<uint64_t>(r) << kRankNoiseShift;
    // Reed valve: a saturating reed buzzes louder than a flue pipe (its limit
    // cycle pins near +-1), so trim the output back toward a flue's loudness.
    pipe.reed = std::clamp(ranks[r].reed, 0.0f, 1.0f);
    pipe.tone_scale = 1.0f - 0.6f * pipe.reed;

    const float footage = ranks[r].footage_mult > 0.01f ? ranks[r].footage_mult : 1.0f;
    const float f0 = base_f0 * footage;
    const float period = static_cast<float>(sr) / f0;
    // Open pipe: positive-feedback comb of one full period (all harmonics).
    // Stopped pipe: negative-feedback comb of half the length, so the impulse
    // response flips sign every base_period samples (period 2*base_period == one
    // fundamental period) and only the odd harmonics survive.
    pipe.base_period = ranks[r].stopped ? 0.5f * period : period;
    pipe.sign = ranks[r].stopped ? -1.0f : 1.0f;

    // Loop lowpass: brightness -> feedback pole a (y += (1-a)(x-y)). A reed
    // pipe radiates far more upper partials than a flue, so the reed valve also
    // opens the loop (keeps the harmonics its saturation generates).
    const float reed01 = std::clamp(ranks[r].reed, 0.0f, 1.0f);
    const float a =
        (1.0f - std::clamp(ranks[r].brightness, 0.0f, 1.0f)) * 0.7f * (1.0f - 0.6f * reed01);
    pipe.alpha = 1.0f - a;
    pipe.lp_state = 0.0f;
    // In-loop DC blocker pole (~8 Hz): kills the open comb's DC pressure mode.
    pipe.dc_x1 = 0.0f;
    pipe.dc_y1 = 0.0f;
    pipe.dc_r = 1.0f - static_cast<float>(kTwoPi * 8.0 / sr);
    // Tuning: compensate the loop filter's exact phase delay at the FUNDAMENTAL
    // (f0 either way) plus the one-sample feedback path, so the sounding pitch
    // matches the rank for both the open and the (half-length) stopped comb.
    const float omega = kTwoPi / period;
    const float tau_lp =
        std::atan2(a * std::sin(omega), 1.0f - a * std::cos(omega)) / std::max(omega, 1.0e-6f);
    // The in-loop DC blocker leads in phase near the fundamental, shortening the
    // effective loop and sounding the pipe sharp in the bass; compensate its
    // exact phase delay at f0 too: H_dc = (1 - z^-1)/(1 - r z^-1).
    const float phase_dc =
        std::atan2(std::sin(omega), 1.0f - std::cos(omega)) -
        std::atan2(pipe.dc_r * std::sin(omega), 1.0f - pipe.dc_r * std::cos(omega));
    const float tau_dc = phase_dc / std::max(omega, 1.0e-6f);
    pipe.comp = 1.0f + tau_lp - tau_dc;

    // Resonator Q: t60 sets the undriven ring (no keyboard stretch — a pipe is
    // sustained by wind, not a decaying string).
    pipe.loop_gain = loop_gain_for(pipe.base_period, sr, t60);
    pipe.release_gain = loop_gain_for(pipe.base_period, sr, rel_t60);

    // Steady jet drive that holds the tone. A white-noise-driven resonator of
    // pole radius g settles to ~ drive / sqrt(1 - g^2), so scaling the drive by
    // sqrt(1 - g^2) pins the SUSTAINED level independent of the ring time.
    const float loss = std::sqrt(std::max(0.0f, 1.0f - pipe.loop_gain * pipe.loop_gain));
    pipe.breath_level = std::clamp(params.breath, 0.0f, 1.0f) * loss * fill_amp * kBreathGain;
    // High-pass the breath just below the fundamental (0.6*f0) so it energises
    // the harmonics, not the DC comb mode (open pipe) or a sub-audio wander.
    pipe.breath_hp_state = 0.0f;
    pipe.breath_hp_alpha =
        std::clamp(1.0f - std::exp(-kTwoPi * 0.6f * f0 / static_cast<float>(sr)), 0.0f, 1.0f);

    // Chiff: a bright onset burst added post-loop, decaying through a one-pole.
    pipe.chiff_level = std::clamp(params.chiff, 0.0f, 1.0f) * fill_amp * kChiffGain;
    pipe.chiff_coeff = std::exp(
        -1.0f / std::max(1.0f, static_cast<float>(std::max(0.5f, params.chiff_ms) * 0.001 * sr)));

    // Circular span for this rank: the loop period plus bend-down headroom
    // (+2 semitones ~= x1.13) and the interpolator's stencil margin.
    pipe.size = std::min(rank_capacity_, static_cast<int>(pipe.base_period * 1.3f) + 8);
    pipe.write_index = 0;
    // Pre-fill the loop with the seeded onset burst so the pipe speaks at full
    // amplitude immediately (the Karplus-Strong trick) instead of swelling in.
    if (pipe.buffer != nullptr) {
      for (int i = 0; i < pipe.size; ++i) {
        pipe.buffer[static_cast<size_t>(i)] =
            fill_amp *
            noise_.bipolar_at(kFillIndexBase + pipe.noise_offset + static_cast<uint64_t>(i));
      }
    }
  }
}

float PipeOrganVoiceCore::render(float pitch_ratio) noexcept {
  if (slab_ == nullptr) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  float mix = 0.0f;
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    if (pipe.buffer == nullptr || pipe.size < 8) continue;

    // Chiff: decaying bright onset noise on top of the pitched tone.
    float chiff = 0.0f;
    if (pipe.chiff_level > 1.0e-5f) {
      chiff =
          pipe.chiff_level * noise_.bipolar_at(kChiffIndexBase + pipe.noise_offset + drive_index_);
      pipe.chiff_level *= pipe.chiff_coeff;
    }

    // Steady jet turbulence replacing the loop loss, high-passed so the open
    // pipe's DC comb mode is not pumped (drive = noise - lowpass(noise)).
    const float raw = noise_.bipolar_at(kBreathIndexBase + pipe.noise_offset + drive_index_);
    pipe.breath_hp_state += pipe.breath_hp_alpha * (raw - pipe.breath_hp_state);
    const float drive = pipe.breath_level * (raw - pipe.breath_hp_state);

    // pitch_ratio scales the frequency, so it divides the loop delay.
    const float delay =
        std::clamp(pipe.base_period / ratio - pipe.comp, 1.0f, static_cast<float>(pipe.size - 4));
    const int delay_q8 = static_cast<int>(delay * 256.0f);

    const float fb = pipe.sign * pipe.loop_gain * pipe.lp_state;
    float in = drive + fb;
    // Reed valve: fold a saturating reed into the loop. tanh(kReedDrive*in) has
    // small-signal gain > 1 (the reed starts to oscillate) but saturates < 1
    // (the limit cycle is bounded), so the pipe buzzes with a bright spectrum
    // and the loop stays stable without an explicit clamp.
    if (pipe.reed > 0.0f) {
      in = (1.0f - pipe.reed) * in + pipe.reed * std::tanh(kReedDrive * in);
    }
    // DC-block the signal entering the delay line so the open comb's DC mode
    // cannot charge up (radiation high-pass).
    const float in_dc = in - pipe.dc_x1 + pipe.dc_r * pipe.dc_y1;
    pipe.dc_x1 = in;
    pipe.dc_y1 = in_dc;
    const float out = rt::lagrange3_fractional_delay(pipe.buffer, static_cast<size_t>(pipe.size),
                                                     pipe.write_index, delay_q8, in_dc);
    pipe.lp_state += pipe.alpha * (out - pipe.lp_state);
    mix += pipe.mix * pipe.tone_scale * (out + chiff);
  }
  ++drive_index_;
  return mix;
}

void PipeOrganVoiceCore::release() noexcept {
  // Wind off: stop driving the jets and damp every column to a short tail.
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.loop_gain = std::min(pipe.loop_gain, pipe.release_gain);
    pipe.breath_level = 0.0f;
    pipe.chiff_level = 0.0f;
  }
}

void PipeOrganVoiceCore::kill() noexcept {
  for (int r = 0; r < rank_count_; ++r) {
    Rank& pipe = ranks_[static_cast<size_t>(r)];
    pipe.loop_gain = 0.0f;
    pipe.lp_state = 0.0f;
    pipe.breath_level = 0.0f;
    pipe.chiff_level = 0.0f;
  }
}

// ---------------------------------------------------------------------------
// OrganWindSupply
// ---------------------------------------------------------------------------

void OrganWindSupply::prepare(double sample_rate, float tremulant_rate_hz, float tremulant_depth,
                              float wind_sag) noexcept {
  sr_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float rate = tremulant_rate_hz > 0.0f ? tremulant_rate_hz : 0.0f;
  const float depth = std::clamp(tremulant_depth, 0.0f, 1.0f);
  sag_depth_ = std::clamp(wind_sag, 0.0f, 1.0f);

  trem_phase_ = 0.0;
  trem_inc_ = rate > 0.0f ? kTwoPi * static_cast<double>(rate) / sr_ : 0.0;
  // A full-depth tremulant undulates the pressure by ~3% -> ~25 cents of pitch
  // and ~2.5 dB of level, the gentle wobble of a real wind tremulant.
  trem_pitch_cents_ = (rate > 0.0f) ? 25.0f * depth : 0.0f;
  trem_amp_ = (rate > 0.0f) ? 0.28f * depth : 0.0f;

  // Wind sag follower: pressure relaxes toward its target over ~80 ms, so a
  // chord onset dips then the regulator catches up.
  pressure_ = 1.0f;
  follow_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.08 * sr_)));

  active_ = trem_inc_ > 0.0 || sag_depth_ > 0.0f;
}

void OrganWindSupply::reset() noexcept {
  trem_phase_ = 0.0;
  pressure_ = 1.0f;
}

OrganWindSupply::State OrganWindSupply::process(int demand) noexcept {
  State s;
  if (!active_) return s;

  // Wind sag: demand (sounding pipes) pulls the pressure down through a
  // saturating load, smoothed by the regulator follower.
  if (sag_depth_ > 0.0f) {
    const float load = static_cast<float>(std::max(0, demand));
    const float target = 1.0f - sag_depth_ * (load / (load + 6.0f));
    pressure_ += follow_coeff_ * (target - pressure_);
  }

  // Tremulant: a slow undulation of the pressure on top of the sag.
  float trem = 0.0f;
  if (trem_inc_ > 0.0) {
    trem = std::sin(static_cast<float>(trem_phase_));
    trem_phase_ += trem_inc_;
    if (trem_phase_ >= kTwoPi) trem_phase_ -= kTwoPi;
  }

  const float pitch_cents = trem_pitch_cents_ * trem;
  // Sag drops pitch a touch as pressure falls (a slack pipe speaks flat).
  const float sag_pitch_cents = (pressure_ - 1.0f) * 30.0f;
  s.pitch_ratio = std::exp2((pitch_cents + sag_pitch_cents) * (1.0f / 1200.0f));
  s.gain = pressure_ * (1.0f + trem_amp_ * trem);
  return s;
}

}  // namespace sonare::midi::synth
