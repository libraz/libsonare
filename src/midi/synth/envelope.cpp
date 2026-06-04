#include "midi/synth/envelope.h"

#include <algorithm>
#include <cmath>

#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

namespace {

// Attack aims at kAttackTarget (> 1) and switches to hold/decay when the level
// crosses 1.0 — the classic analog-EG overshoot shape. The one-pole crossing
// time is t1 = tau * ln(T / (T - 1)); we size tau so t1 == attack_ms.
constexpr float kAttackTarget = 1.3f;
const float kAttackTauScale = std::log(kAttackTarget / (kAttackTarget - 1.0f));  // ~1.466

// Decay/release "time" means time to come within ~5% of the target
// (3 time constants), so the audible move completes in the configured time.
constexpr float kDecayTauScale = 3.0f;

float stage_rate(double sample_rate, float time_ms, float tau_scale) noexcept {
  if (time_ms <= 0.0f) return 1.0f;  // instantaneous
  return sonare::time_to_attack_release_rate_f(sample_rate, time_ms / tau_scale);
}

int32_t stage_samples(double sample_rate, float time_ms) noexcept {
  if (time_ms <= 0.0f || sample_rate <= 0.0) return 0;
  const double frames = sample_rate * static_cast<double>(time_ms) * 0.001;
  return static_cast<int32_t>(std::lround(frames));
}

}  // namespace

void DahdsrEnvelope::configure(double sample_rate, const DahdsrConfig& config) noexcept {
  sustain_ = std::clamp(config.sustain, 0.0f, 1.0f);
  attack_rate_ = stage_rate(sample_rate, config.attack_ms, kAttackTauScale);
  decay_rate_ = stage_rate(sample_rate, config.decay_ms, kDecayTauScale);
  release_rate_ = stage_rate(sample_rate, config.release_ms, kDecayTauScale);
  delay_samples_ = stage_samples(sample_rate, config.delay_ms);
  hold_samples_ = stage_samples(sample_rate, config.hold_ms);
}

void DahdsrEnvelope::note_on() noexcept {
  // Retrigger from the current level so overlapping notes stay click-free.
  if (delay_samples_ > 0 && level_ <= kSilenceLevel) {
    stage_ = Stage::kDelay;
    stage_counter_ = delay_samples_;
  } else {
    stage_ = Stage::kAttack;
  }
}

void DahdsrEnvelope::note_off() noexcept {
  if (stage_ == Stage::kIdle) return;
  stage_ = Stage::kRelease;
}

void DahdsrEnvelope::kill() noexcept {
  stage_ = Stage::kIdle;
  level_ = 0.0f;
}

float DahdsrEnvelope::next() noexcept {
  switch (stage_) {
    case Stage::kIdle:
      return 0.0f;
    case Stage::kDelay:
      if (--stage_counter_ <= 0) stage_ = Stage::kAttack;
      return level_;
    case Stage::kAttack:
      level_ += attack_rate_ * (kAttackTarget - level_);
      if (level_ >= 1.0f) {
        level_ = 1.0f;
        if (hold_samples_ > 0) {
          stage_ = Stage::kHold;
          stage_counter_ = hold_samples_;
        } else {
          stage_ = Stage::kDecay;
        }
      }
      return level_;
    case Stage::kHold:
      if (--stage_counter_ <= 0) stage_ = Stage::kDecay;
      return level_;
    case Stage::kDecay:
      level_ += decay_rate_ * (sustain_ - level_);
      // Within the 5% landing window (relative to full scale) -> sustain.
      if (level_ - sustain_ <= 0.05f * (1.0f - sustain_) || decay_rate_ >= 1.0f) {
        // Keep the exponential glide; only pin once effectively converged.
        if (level_ - sustain_ <= 1.0e-3f || decay_rate_ >= 1.0f) {
          level_ = sustain_;
          stage_ = Stage::kSustain;
        }
      }
      if (stage_ == Stage::kSustain && sustain_ <= kSilenceLevel) {
        // A zero-sustain envelope (percussive) ends at the decay floor.
        kill();
      }
      return level_;
    case Stage::kSustain:
      level_ = sustain_;
      return level_;
    case Stage::kRelease:
      level_ += release_rate_ * (0.0f - level_);
      if (level_ <= kSilenceLevel) kill();
      return level_;
  }
  return 0.0f;
}

int64_t DahdsrEnvelope::release_tail_samples(double sample_rate, float release_ms) noexcept {
  if (!(sample_rate > 0.0) || release_ms <= 0.0f) return 0;
  // level(t) = exp(-t / tau) with tau = release_ms / kDecayTauScale; the tail
  // ends when level == kSilenceLevel -> t = tau * ln(1 / kSilenceLevel).
  const double tau_s = (static_cast<double>(release_ms) * 0.001) / kDecayTauScale;
  const double tail_s = tau_s * std::log(1.0 / static_cast<double>(kSilenceLevel));
  return static_cast<int64_t>(std::ceil(tail_s * sample_rate));
}

}  // namespace sonare::midi::synth
