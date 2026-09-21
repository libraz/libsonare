#include "effects/modulation/rotary.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

namespace {

constexpr float kMaxDepthMs = 20.0f;

/// The fraction of the remaining gap a first-order glide closes per sample.
/// A non-positive time constant is no glide at all: the rotor arrives at once.
float glide_coeff(float tau_s, double sample_rate) noexcept {
  if (!(tau_s > 0.0f)) return 1.0f;
  return static_cast<float>(1.0 - std::exp(-1.0 / (static_cast<double>(tau_s) * sample_rate)));
}

}  // namespace

Rotary::Rotary(RotaryConfig config) : config_(config) {
  config_.rate_hz = std::max(0.0f, config_.rate_hz);
  config_.drum_rate_hz = std::max(0.0f, config_.drum_rate_hz);
  config_.depth_ms = std::clamp(config_.depth_ms, 0.0f, kMaxDepthMs);
  config_.tremolo = std::clamp(config_.tremolo, 0.0f, 1.0f);
  config_.stereo_spread = std::clamp(config_.stereo_spread, 0.0f, 1.0f);
  config_.accel_tau_s = std::max(0.0f, config_.accel_tau_s);
  config_.decel_tau_s = std::max(0.0f, config_.decel_tau_s);
  config_.undershoot_hz = std::max(0.0f, config_.undershoot_hz);
  config_.drum_undershoot_hz = std::max(0.0f, config_.drum_undershoot_hz);
}

void Rotary::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // One-pole lowpass crossover coefficient.
  const double x =
      std::exp(-2.0 * ::sonare::constants::kPiD * static_cast<double>(kCrossoverHz) / sample_rate_);
  lp_coeff_ = static_cast<float>(x);
  accel_coeff_ = glide_coeff(config_.accel_tau_s, sample_rate_);
  decel_coeff_ = glide_coeff(config_.decel_tau_s, sample_rate_);
  const int max_delay =
      static_cast<int>(sample_rate_ * (2.0 * static_cast<double>(kMaxDepthMs) + 2.0) * 0.001) + 1;
  for (int ch = 0; ch < 2; ++ch) {
    horn_lfo_[ch].prepare(sample_rate_);
    drum_lfo_[ch].prepare(sample_rate_);
    horn_delay_[ch].prepare(max_delay);
    drum_delay_[ch].prepare(max_delay);
  }
  reset();
}

float Rotary::advance_rotor(float& rate, float target, float undershoot) const noexcept {
  if (target > rate) {
    // Speeding up aims short by the measured offset, and not at all where the target is nearer.
    const float aim = std::max(rate, target - undershoot);
    rate += (aim - rate) * accel_coeff_;
  } else {
    rate += (target - rate) * decel_coeff_;
  }
  return rate;
}

void Rotary::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float trem = std::clamp(config_.tremolo, 0.0f, 1.0f);
  const float base = config_.depth_ms;
  const float swing = config_.depth_ms;
  const float ms_to_samp = 0.001f * static_cast<float>(sample_rate_);
  // Stereo-pair processor: the horn/drum rotor state exists for two planes only,
  // so planes beyond the pair pass through dry (see the registry's
  // stereoPairOnly classification).
  const int active = std::min(num_channels, 2);
  for (int i = 0; i < num_samples; ++i) {
    const float horn_rate = advance_rotor(horn_rate_, config_.rate_hz, config_.undershoot_hz);
    const float drum_rate =
        advance_rotor(drum_rate_, config_.drum_rate_hz, config_.drum_undershoot_hz);
    for (int ch = 0; ch < 2; ++ch) {
      horn_lfo_[ch].set_rate_hz(horn_rate);
      drum_lfo_[ch].set_rate_hz(drum_rate);
    }
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      // Crossover: one-pole lowpass = drum band, remainder = horn band.
      lp_state_[ch] = in + lp_coeff_ * (lp_state_[ch] - in);
      const float drum_in = lp_state_[ch];
      const float horn_in = in - drum_in;
      const float horn_mod = horn_lfo_[ch].process();
      const float drum_mod = drum_lfo_[ch].process();
      const float horn_delay = (base + swing * horn_mod) * ms_to_samp;
      const float drum_delay = (base + 0.7f * swing * drum_mod) * ms_to_samp;
      // Tremolo attenuates from unity rather than swinging around it. The LFO
      // spans [-1, 1], so a gain of 1 + depth * mod would peak at 1 + depth --
      // at the default depth that is +3.5 dB of makeup nobody asked for, and it
      // scales with the depth control. Mapping the LFO to [1 - depth, 1] keeps
      // the same modulation shape and leaves the loudest point at unity.
      const float horn_gain = 1.0f - trem * 0.5f * (1.0f - horn_mod);
      // The bass rotor's tremolo is shallower (the drum baffle throws less).
      const float drum_gain = 1.0f - 0.6f * trem * 0.5f * (1.0f - drum_mod);
      const float horn = horn_delay_[ch].process(horn_in, horn_delay) * horn_gain;
      const float drum = drum_delay_[ch].process(drum_in, drum_delay) * drum_gain;
      channels[ch][i] = dry * in + wet * (horn + drum);
    }
  }
  discard_non_finite();
}

void Rotary::discard_non_finite() noexcept {
  // The rotor delay lines are fed by the crossover output alone, so a
  // non-finite sample leaves them within one line length.
  if (discard_run_if_non_finite(lp_state_.begin(), lp_state_.end(), 0.0f)) {
    note_non_finite_discard();
  }
}

void Rotary::reset() {
  lp_state_ = {0.0f, 0.0f};
  // A rotor comes back up to speed rather than spinning from rest, so no glide is in flight here.
  horn_rate_ = config_.rate_hz;
  drum_rate_ = config_.drum_rate_hz;
  // Anti-phase L/R (scaled by the stereo spread) gives the swirling image.
  const double offset = 0.5 * static_cast<double>(config_.stereo_spread);
  horn_lfo_[0].reset(0.0);
  horn_lfo_[1].reset(offset);
  drum_lfo_[0].reset(0.0);
  drum_lfo_[1].reset(offset);
  for (int ch = 0; ch < 2; ++ch) {
    horn_lfo_[ch].set_rate_hz(horn_rate_);
    drum_lfo_[ch].set_rate_hz(drum_rate_);
    horn_delay_[ch].reset();
    drum_delay_[ch].reset();
  }
}

bool Rotary::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      return true;
    case 1:
      config_.depth_ms = std::clamp(value, 0.0f, kMaxDepthMs);
      return true;
    case 2:
      config_.tremolo = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    case 4:
      config_.drum_rate_hz = std::max(0.0f, value);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Rotary::parameter_descriptors() const {
  return {{"rateHz", 0}, {"depthMs", 1}, {"tremolo", 2}, {"dryWet", 3}, {"drumRateHz", 4}};
}

}  // namespace sonare::effects::modulation
