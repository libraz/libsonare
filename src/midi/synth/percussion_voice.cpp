#include "midi/synth/percussion_voice.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "midi/synth/bessel.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kInvSqrt2;
using sonare::constants::kPi;
using sonare::constants::kTwoPi;
/// Noise draws live far above any other per-voice index range.
constexpr uint64_t kNoiseIndexBase = 1ull << 20;
/// Wire-rattle draws live above the noise-layer range so the two streams
/// stay decorrelated.
constexpr uint64_t kWireIndexBase = 1ull << 24;
/// Shimmer draws live above the wire-rattle range.
constexpr uint64_t kShimmerIndexBase = 1ull << 28;
/// PhISEM collision-probability draws and the particle-noise draws live in two
/// disjoint ranges above the shimmer range so all streams stay decorrelated.
constexpr uint64_t kPhisemProbIndexBase = 1ull << 30;
constexpr uint64_t kPhisemNoiseIndexBase = 1ull << 31;
/// Random bead collisions per bean per unit shake energy per second.
SONARE_TUNABLE(kPhisemCollisionRate, 100.0f);

float radius_for(double sample_rate, float t60_s) noexcept {
  return std::exp(-6.907755279f / (static_cast<float>(sample_rate) * std::max(0.005f, t60_s)));
}

}  // namespace

void PercussionVoiceCore::start(const PercussionPatchParams& params, double sample_rate,
                                uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  noise_index_ = 0;

  const float base_hz = params.base_freq_hz > 0.0f ? params.base_freq_hz : note_to_hz(note);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;

  // Membrane modes: harder hits excite the upper ring modes a bit more.
  num_modes_ = std::clamp(params.num_modes, 0, kMaxPercussionModes);
  tone_gain_ = std::max(0.0f, params.tone_gain);
  const float nyquist_limit = 0.45f * static_cast<float>(sr);
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    const float ratio = params.mode_ratios[static_cast<size_t>(k)];
    const float freq = base_hz * std::max(0.01f, ratio);
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    if (ratio <= 0.0f || freq >= nyquist_limit) {
      mode = Mode{};
      continue;
    }
    mode.omega = kTwoPi * freq / static_cast<float>(sr);
    // Upper membrane modes die faster than the fundamental (1/ratio scaling).
    mode.r = radius_for(sr, std::max(0.005f, params.mode_decay_s) / std::max(1.0f, ratio));
    const float strike = k == 0 ? 1.0f : (0.4f + 0.4f * vel01) / static_cast<float>(k + 1);
    // Strike-point weighting: each membrane mode is excited by the value of
    // its shape J_m(alpha_mn * r) * cos(m * theta) at the strike. A centre
    // hit (strike_r == 0) is the legacy uniform excitation.
    float strike_pos = 1.0f;
    if (params.strike_r > 0.0f) {
      const int m = static_cast<int>(params.mode_m[static_cast<size_t>(k)]);
      const float arg = params.mode_alpha[static_cast<size_t>(k)] * params.strike_r;
      strike_pos =
          std::abs(bessel_j(m, arg) * std::cos(static_cast<float>(m) * params.strike_theta));
    }
    mode.gain = strike * std::sin(mode.omega) * strike_pos;
  }
  for (int k = num_modes_; k < kMaxPercussionModes; ++k) modes_[static_cast<size_t>(k)] = Mode{};

  // Descending pitch envelope.
  drop_state_ = std::max(0.0f, params.pitch_drop);
  drop_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.pitch_drop_ms) * 0.001f * static_cast<float>(sr)));
  cached_ratio_ = 0.0f;
  excite_ = num_modes_ > 0;

  // Noise layer.
  noise_level_ = std::max(0.0f, params.noise_gain) * (0.6f + 0.4f * vel01);
  noise_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.noise_decay_ms) * 0.001f * static_cast<float>(sr)));
  noise_output_ = params.noise_output;
  noise_filter_.prepare(sr);
  noise_filter_.set(params.noise_cutoff_hz, std::max(0.5f, params.noise_q));
  noise_filter_.reset();

  // Radiated upper bound over every noise stream. Butterworth Q, because this
  // is a ceiling and a resonant one would put back a peak of its own.
  noise_air_hz_ = params.noise_air_hz > 0.0f
                      ? std::min(params.noise_air_hz, 0.45f * static_cast<float>(sr))
                      : 0.0f;
  if (noise_air_hz_ > 0.0f) {
    for (TptSvf* air : {&noise_air_, &wire_air_, &shimmer_air_}) {
      air->prepare(sr);
      air->set(noise_air_hz_, kInvSqrt2);
      air->reset();
    }
  }

  // Shell resonance: the summed hit rings through the drum body. A note-tracked
  // 0 Hz spec is taken to mean "track the struck key" so one tom patch voices
  // every tom size.
  const int shell_count = std::clamp(params.shell_num_modes, 0, kMaxShellModes);
  std::array<BodyResonator::Spec, kMaxShellModes> shell_specs{};
  for (int k = 0; k < shell_count; ++k) {
    const float spec_hz = params.shell_freq_hz[static_cast<size_t>(k)];
    shell_specs[static_cast<size_t>(k)] = {
        spec_hz > 0.0f ? spec_hz : base_hz,
        std::max(0.005f, params.shell_t60_s[static_cast<size_t>(k)]),
        params.shell_weight[static_cast<size_t>(k)]};
  }
  shell_.start_specs(shell_specs.data(), shell_count, sr, params.shell_mix);

  // Snare wire rattle: gated noise driven by the membrane crossing the wire
  // contact threshold. Voiced through a dedicated high-pass.
  wire_buzz_ = std::max(0.0f, params.wire_buzz);
  wire_threshold_ = std::max(0.0f, params.wire_threshold);
  wire_vel01_ = vel01;
  wire_index_ = 0;
  wire_filter_.prepare(sr);
  wire_filter_.set(params.wire_cutoff_hz, 0.9f);
  wire_filter_.reset();

  // Nonlinear cymbal shimmer: the membrane energy pumps a high shimmer band
  // through a slow attack follower (the buildup lag).
  shimmer_ = std::max(0.0f, params.shimmer);
  shimmer_env_ = 0.0f;
  shimmer_attack_coeff_ = 1.0f - std::exp(-1.0f / (std::max(1.0f, params.shimmer_attack_ms) *
                                                   0.001f * static_cast<float>(sr)));
  shimmer_index_ = 0;
  shimmer_filter_.prepare(sr);
  shimmer_filter_.set(params.shimmer_cutoff_hz, 0.7f);
  shimmer_filter_.reset();

  // Stochastic particle excitation (PhISEM). Off when beans == 0 (bit-identical
  // — no draws, no state advance).
  phisem_beans_ = std::max(0.0f, params.phisem_beans);
  phisem_sr_ = static_cast<float>(sr);
  phisem_prob_index_ = 0;
  phisem_noise_index_ = 0;
  phisem_sound_level_ = 0.0f;
  phisem_scrape_phase_ = 0.0f;
  phisem_glide_state_ = 0.0f;
  if (phisem_beans_ > 0.0f) {
    // A shake gesture: the system energy is set by the strike and dies over
    // phisem_energy_ms; each collision bumps the sounding energy, which decays
    // over the short grain time phisem_sound_ms.
    phisem_shake_energy_ = 0.3f + 0.7f * vel01;
    phisem_sys_decay_ = std::exp(
        -1.0f / (std::max(1.0f, params.phisem_energy_ms) * 0.001f * static_cast<float>(sr)));
    phisem_sound_decay_ = std::exp(
        -1.0f / (std::max(0.2f, params.phisem_sound_ms) * 0.001f * static_cast<float>(sr)));
    phisem_rate_ = kPhisemCollisionRate / static_cast<float>(sr);
    phisem_scrape_inc_ =
        params.phisem_scrape_hz > 0.0f ? params.phisem_scrape_hz / static_cast<float>(sr) : 0.0f;
    phisem_res_hz_ = params.phisem_res_hz;
    phisem_res_q_ = std::max(0.5f, params.phisem_res_q);
    phisem_glide_state_ = params.phisem_pitch_glide;
    phisem_glide_coeff_ = std::exp(
        -1.0f / (std::max(1.0f, params.phisem_energy_ms) * 0.001f * static_cast<float>(sr)));
    phisem_filter_.prepare(sr);
    if (phisem_res_hz_ > 0.0f) {
      const float c = phisem_res_hz_ * (1.0f + phisem_glide_state_);
      phisem_filter_.set(std::clamp(c, 20.0f, 0.45f * static_cast<float>(sr)), phisem_res_q_);
    }
    phisem_filter_.reset();
  }
}

float PercussionVoiceCore::render(float pitch_ratio) noexcept {
  float mix = 0.0f;

  if (num_modes_ > 0) {
    // Tone layer with the descending strike pitch folded into the ratio.
    float ratio = pitch_ratio * (1.0f + drop_state_);
    if (drop_state_ > 0.0f) {
      drop_state_ *= drop_coeff_;
      if (drop_state_ < 1.0e-3f) drop_state_ = 0.0f;
    }
    if (ratio != cached_ratio_) {
      cached_ratio_ = ratio;
      for (int k = 0; k < num_modes_; ++k) {
        Mode& mode = modes_[static_cast<size_t>(k)];
        if (mode.gain == 0.0f && mode.r == 0.0f) continue;
        const float w = std::min(mode.omega * ratio, 0.95f * kPi);
        mode.a1 = 2.0f * mode.r * std::cos(w);
        mode.a2 = -mode.r * mode.r;
      }
    }
    const float x = excite_ ? 1.0f : 0.0f;
    excite_ = false;
    float tone = 0.0f;
    for (int k = 0; k < num_modes_; ++k) {
      Mode& mode = modes_[static_cast<size_t>(k)];
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * x;
      mode.y2 = mode.y1;
      mode.y1 = y;
      tone += y;
    }
    mix += tone_gain_ * tone;

    // Snare wire rattle: while the membrane swing exceeds the contact
    // threshold the wires buzz against the bottom head. The gate scales with
    // how far the head is over threshold and with strike velocity, so harder
    // hits rattle louder and (because the membrane stays over threshold
    // longer) longer.
    if (wire_buzz_ > 0.0f) {
      const float contact = std::abs(tone) - wire_threshold_;
      const float gate = contact > 0.0f ? std::min(contact * 8.0f, 1.0f) : 0.0f;
      const float n =
          noise_.bipolar_at(kWireIndexBase + wire_index_++) * gate * wire_vel01_ * wire_buzz_;
      const float wire = wire_filter_.process(n).hp;
      mix += noise_air_hz_ > 0.0f ? wire_air_.process(wire).lp : wire;
    }

    // Nonlinear shimmer: the quadratic membrane energy (tone^2) drives a high
    // shimmer band through a slow-attack follower, so the wash swells after
    // the strike and rides the inharmonic ring. One-way, so it stays stable.
    if (shimmer_ > 0.0f) {
      shimmer_env_ += (tone * tone - shimmer_env_) * shimmer_attack_coeff_;
      const float n = noise_.bipolar_at(kShimmerIndexBase + shimmer_index_++);
      const float wash = shimmer_filter_.process(n * shimmer_env_ * shimmer_).hp;
      mix += noise_air_hz_ > 0.0f ? shimmer_air_.process(wash).lp : wash;
    }
  }

  if (noise_level_ > 1.0e-5f) {
    const float burst = noise_.bipolar_at(kNoiseIndexBase + noise_index_++) * noise_level_;
    noise_level_ *= noise_coeff_;
    const TptSvf::Outputs out = noise_filter_.process(burst);
    float voiced = 0.0f;
    switch (noise_output_) {
      case SynthFilterOutput::kHighpass:
        voiced = out.hp;
        break;
      case SynthFilterOutput::kBandpass:
        voiced = out.bp;
        break;
      case SynthFilterOutput::kLowpass:
        voiced = out.lp;
        break;
    }
    mix += noise_air_hz_ > 0.0f ? noise_air_.process(voiced).lp : voiced;
  }

  // Stochastic particle excitation (PhISEM). The shake energy decays over the
  // gesture; bead/ridge collisions bump the sounding energy that scales a single
  // noise source, optionally rung through a gourd resonance (cuica glides it).
  if (phisem_beans_ > 0.0f) {
    phisem_shake_energy_ *= phisem_sys_decay_;
    bool collide = false;
    // Scrape (guiro/cuica): a ridge passes under the scraper each period.
    if (phisem_scrape_inc_ > 0.0f) {
      phisem_scrape_phase_ += phisem_scrape_inc_;
      if (phisem_scrape_phase_ >= 1.0f) {
        phisem_scrape_phase_ -= 1.0f;
        collide = true;
      }
    }
    // Random bead collisions on top; the rate falls as the shake dies out.
    const float p = phisem_beans_ * phisem_shake_energy_ * phisem_rate_;
    if (noise_.unipolar_at(kPhisemProbIndexBase + phisem_prob_index_++) < p) collide = true;
    if (collide) {
      phisem_sound_level_ = std::min(phisem_sound_level_ + phisem_shake_energy_ * 0.6f, 4.0f);
    }
    float particle =
        noise_.bipolar_at(kPhisemNoiseIndexBase + phisem_noise_index_++) * phisem_sound_level_;
    phisem_sound_level_ *= phisem_sound_decay_;
    if (phisem_res_hz_ > 0.0f) {
      // Cuica pitch glide: ease the resonance centre back to res_hz.
      if (phisem_glide_state_ != 0.0f) {
        phisem_glide_state_ *= phisem_glide_coeff_;
        if (std::abs(phisem_glide_state_) < 1.0e-3f) phisem_glide_state_ = 0.0f;
        const float c = phisem_res_hz_ * (1.0f + phisem_glide_state_);
        phisem_filter_.set(std::clamp(c, 20.0f, 0.45f * phisem_sr_), phisem_res_q_);
      }
      particle = phisem_filter_.process(particle).bp;
    }
    mix += particle;
  }

  if (shell_.active()) mix = shell_.process(mix);

  return mix;
}

void PercussionVoiceCore::kill() noexcept {
  for (Mode& mode : modes_) {
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    mode.gain = 0.0f;
  }
  num_modes_ = 0;
  noise_level_ = 0.0f;
  excite_ = false;
  noise_air_hz_ = 0.0f;
  noise_air_.reset();
  wire_air_.reset();
  shimmer_air_.reset();
  shell_.reset();
  wire_buzz_ = 0.0f;
  wire_filter_.reset();
  shimmer_ = 0.0f;
  shimmer_env_ = 0.0f;
  shimmer_filter_.reset();
  phisem_beans_ = 0.0f;
  phisem_shake_energy_ = 0.0f;
  phisem_sound_level_ = 0.0f;
  phisem_filter_.reset();
}

}  // namespace sonare::midi::synth
