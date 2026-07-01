#include "effects/modulation/rotary.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

namespace {

constexpr float kMaxDepthMs = 20.0f;

}  // namespace

Rotary::Rotary(RotaryConfig config) : config_(config) {
  config_.rate_hz = std::max(0.0f, config_.rate_hz);
  config_.depth_ms = std::clamp(config_.depth_ms, 0.0f, kMaxDepthMs);
  config_.tremolo = std::clamp(config_.tremolo, 0.0f, 1.0f);
  config_.stereo_spread = std::clamp(config_.stereo_spread, 0.0f, 1.0f);
}

void Rotary::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  // One-pole lowpass crossover coefficient.
  const double x =
      std::exp(-2.0 * ::sonare::constants::kPiD * static_cast<double>(kCrossoverHz) / sample_rate_);
  lp_coeff_ = static_cast<float>(x);
  const int max_delay =
      static_cast<int>(sample_rate_ * (2.0 * static_cast<double>(kMaxDepthMs) + 2.0) * 0.001) + 1;
  for (int ch = 0; ch < 2; ++ch) {
    horn_lfo_[ch].prepare(sample_rate_);
    drum_lfo_[ch].prepare(sample_rate_);
    horn_lfo_[ch].set_rate_hz(config_.rate_hz);
    drum_lfo_[ch].set_rate_hz(config_.rate_hz * kDrumRateRatio);
    horn_delay_[ch].prepare(max_delay);
    drum_delay_[ch].prepare(max_delay);
  }
  reset();
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
  const int active = std::min(num_channels, 2);
  for (int i = 0; i < num_samples; ++i) {
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
      const float horn = horn_delay_[ch].process(horn_in, horn_delay) * (1.0f + trem * horn_mod);
      // The bass rotor's tremolo is shallower (the drum baffle throws less).
      const float drum =
          drum_delay_[ch].process(drum_in, drum_delay) * (1.0f + 0.6f * trem * drum_mod);
      channels[ch][i] = dry * in + wet * (horn + drum);
    }
  }
}

void Rotary::reset() {
  lp_state_ = {0.0f, 0.0f};
  // Anti-phase L/R (scaled by the stereo spread) gives the swirling image.
  const double offset = 0.5 * static_cast<double>(config_.stereo_spread);
  horn_lfo_[0].reset(0.0);
  horn_lfo_[1].reset(offset);
  drum_lfo_[0].reset(0.0);
  drum_lfo_[1].reset(offset);
  for (int ch = 0; ch < 2; ++ch) {
    horn_delay_[ch].reset();
    drum_delay_[ch].reset();
  }
}

bool Rotary::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      horn_lfo_[0].set_rate_hz(config_.rate_hz);
      horn_lfo_[1].set_rate_hz(config_.rate_hz);
      drum_lfo_[0].set_rate_hz(config_.rate_hz * kDrumRateRatio);
      drum_lfo_[1].set_rate_hz(config_.rate_hz * kDrumRateRatio);
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
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> Rotary::parameter_descriptors() const {
  return {{"rateHz", 0}, {"depthMs", 1}, {"tremolo", 2}, {"dryWet", 3}};
}

}  // namespace sonare::effects::modulation
