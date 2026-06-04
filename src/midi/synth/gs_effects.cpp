#include "midi/synth/gs_effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonare::midi::synth {

namespace {

effects::reverb::DattorroReverbConfig reverb_config(const GsEffectsConfig& cfg) {
  effects::reverb::DattorroReverbConfig rc;
  rc.decay = std::clamp(cfg.reverb_decay, 0.0f, 0.98f);
  rc.damping = std::clamp(cfg.reverb_damping, 0.0f, 1.0f);
  rc.dry_wet = 1.0f;  // send-return: wet only
  return rc;
}

effects::modulation::ChorusConfig chorus_config(const GsEffectsConfig& cfg) {
  effects::modulation::ChorusConfig cc;
  cc.rate_hz = std::max(0.0f, cfg.chorus_rate_hz);
  cc.depth_ms = std::max(0.0f, cfg.chorus_depth_ms);
  cc.dry_wet = 1.0f;
  return cc;
}

effects::delay::StereoDelayConfig delay_config(const GsEffectsConfig& cfg) {
  effects::delay::StereoDelayConfig dc;
  dc.delay_time_l_ms = std::max(1.0f, cfg.delay_time_ms);
  dc.delay_time_r_ms = std::max(1.0f, cfg.delay_time_ms);
  dc.feedback = std::clamp(cfg.delay_feedback, 0.0f, 0.9f);
  dc.ping_pong = 0.0f;
  dc.dry_wet = 1.0f;
  return dc;
}

}  // namespace

GsEffectBus::GsEffectBus(const GsEffectsConfig& config)
    : config_(config),
      reverb_(reverb_config(config)),
      chorus_(chorus_config(config)),
      delay_(delay_config(config)) {}

void GsEffectBus::prepare(double sample_rate) {
  for (int ch = 0; ch < 2; ++ch) {
    reverb_bus_[ch].assign(kBlockFrames, 0.0f);
    chorus_bus_[ch].assign(kBlockFrames, 0.0f);
    delay_bus_[ch].assign(kBlockFrames, 0.0f);
  }
  reverb_.prepare(sample_rate, kBlockFrames);
  chorus_.prepare(sample_rate, kBlockFrames);
  delay_.prepare(sample_rate, kBlockFrames);
}

void GsEffectBus::reset() {
  reverb_.reset();
  chorus_.reset();
  delay_.reset();
  begin_chunk();
}

void GsEffectBus::begin_chunk() noexcept {
  for (int ch = 0; ch < 2; ++ch) {
    std::memset(reverb_bus_[ch].data(), 0, sizeof(float) * reverb_bus_[ch].size());
    std::memset(chorus_bus_[ch].data(), 0, sizeof(float) * chorus_bus_[ch].size());
    std::memset(delay_bus_[ch].data(), 0, sizeof(float) * delay_bus_[ch].size());
  }
}

void GsEffectBus::render_returns(float* out_l, float* out_r, int n) noexcept {
  if (n <= 0) return;
  n = std::min(n, kBlockFrames);
  if (config_.enable_reverb) {
    float* bus[2] = {reverb_bus_[0].data(), reverb_bus_[1].data()};
    reverb_.process(bus, 2, n);
    for (int i = 0; i < n; ++i) {
      out_l[i] += bus[0][i];
      out_r[i] += bus[1][i];
    }
  }
  if (config_.enable_chorus) {
    float* bus[2] = {chorus_bus_[0].data(), chorus_bus_[1].data()};
    chorus_.process(bus, 2, n);
    for (int i = 0; i < n; ++i) {
      out_l[i] += bus[0][i];
      out_r[i] += bus[1][i];
    }
  }
  if (config_.enable_delay) {
    float* bus[2] = {delay_bus_[0].data(), delay_bus_[1].data()};
    delay_.process(bus, 2, n);
    for (int i = 0; i < n; ++i) {
      out_l[i] += bus[0][i];
      out_r[i] += bus[1][i];
    }
  }
}

int64_t GsEffectBus::tail_samples(double sample_rate) const noexcept {
  if (!(sample_rate > 0.0)) return 0;
  double tail_s = 0.0;
  if (config_.enable_reverb) {
    // The figure-8 tank circulates in roughly 150 ms; energy falls by `decay`
    // per pass. Ring-out to -80 dB: n = ln(1e-4) / ln(decay) passes.
    const double decay = std::clamp(static_cast<double>(config_.reverb_decay), 0.05, 0.98);
    const double passes = std::log(1.0e-4) / std::log(decay);
    tail_s = std::max(tail_s, 0.15 * passes);
  }
  if (config_.enable_delay) {
    const double fb = std::clamp(static_cast<double>(config_.delay_feedback), 0.0, 0.9);
    const double time_s = std::max(1.0f, config_.delay_time_ms) * 0.001;
    const double repeats = fb > 0.0 ? std::log(1.0e-4) / std::log(fb) : 1.0;
    tail_s = std::max(tail_s, time_s * std::max(1.0, repeats));
  }
  if (config_.enable_chorus) {
    tail_s = std::max(tail_s, 0.1);  // modulated delay line ring-out
  }
  return static_cast<int64_t>(std::ceil(tail_s * sample_rate));
}

}  // namespace sonare::midi::synth
