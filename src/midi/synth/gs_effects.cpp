#include "midi/synth/gs_effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

/// System-reverb tank scaling. These are 1.0 multipliers on whatever the host
/// asked for, not values in their own right, so the shipped behaviour is the
/// caller's config exactly. They exist so the voicematch harness can sweep the
/// tank against a reference recording's measured decay: an instrument that is
/// never heard dry (a church organ, an orchestral harp) is fitted against a
/// wet reference, and the room has to be separable from the timbre for that
/// fit to mean anything.
SONARE_TUNABLE(kReverbDecayScale, 1.0f);
SONARE_TUNABLE(kReverbDampingScale, 1.0f);

/// One circulation of the figure-8 tank. Shared by the RT60 -> tank-feedback
/// conversion and the ring-out bound so the two cannot drift apart.
constexpr double kTankPassSeconds = 0.15;

effects::reverb::DattorroReverbConfig reverb_config(const GsEffectsConfig& cfg) {
  effects::reverb::DattorroReverbConfig rc;
  rc.decay = std::clamp(cfg.reverb_decay * kReverbDecayScale, 0.0f, 0.98f);
  rc.damping = std::clamp(cfg.reverb_damping * kReverbDampingScale, 0.0f, 1.0f);
  rc.dry_wet = 1.0f;  // send-return: wet only
  return rc;
}

effects::modulation::ChorusConfig chorus_config(const GsEffectsConfig& cfg) {
  effects::modulation::ChorusConfig cc;
  cc.rate_hz = std::max(0.0f, cfg.chorus_rate_hz);
  cc.depth_ms = std::max(0.0f, cfg.chorus_depth_ms);
  cc.center_delay_ms = std::max(0.0f, cfg.chorus_delay_ms);
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

/// RT60 seconds -> tank feedback, the inverse of the ring-out bound below:
/// -60 dB after t / kTankPassSeconds passes.
float tank_decay_from_rt60(float seconds) noexcept {
  if (!(seconds > 0.0f)) return 0.0f;
  const double decay = std::pow(10.0, -3.0 * kTankPassSeconds / seconds);
  return static_cast<float>(std::clamp(decay, 0.0, 0.98));
}

}  // namespace

GsEffectsConfig gs_effects_config_from(const GsSystemEffects& fx) noexcept {
  GsEffectsConfig cfg;
  cfg.reverb_decay = tank_decay_from_rt60(gs_reverb_time_seconds(fx.reverb_time));
  cfg.reverb_damping = gs_reverb_character_damping(fx.reverb_character);
  cfg.reverb_level = gs_effect_level(fx.reverb_level);
  cfg.reverb_predelay_ms = gs_reverb_predelay_ms(fx.reverb_predelay);
  cfg.reverb_pre_lpf_hz = gs_pre_lpf_cutoff_hz(fx.reverb_pre_lpf);

  cfg.chorus_rate_hz = gs_chorus_rate_hz(fx.chorus_rate);
  cfg.chorus_depth_ms = gs_chorus_depth_ms(fx.chorus_depth);
  cfg.chorus_delay_ms = gs_chorus_delay_ms(fx.chorus_delay);
  cfg.chorus_feedback = gs_chorus_feedback_coefficient(fx.chorus_feedback);
  cfg.chorus_level = gs_effect_level(fx.chorus_level);
  cfg.chorus_pre_lpf_hz = gs_pre_lpf_cutoff_hz(fx.chorus_pre_lpf);
  cfg.chorus_send_to_reverb = gs_effect_level(fx.chorus_send_to_reverb);
  cfg.chorus_send_to_delay = gs_effect_level(fx.chorus_send_to_delay);

  cfg.delay_time_ms = gs_delay_time_ms(fx.delay_time_center);
  cfg.delay_time_ratio_left = gs_delay_time_ratio_percent(fx.delay_time_ratio_left) / 100.0f;
  cfg.delay_time_ratio_right = gs_delay_time_ratio_percent(fx.delay_time_ratio_right) / 100.0f;
  cfg.delay_feedback = gs_delay_feedback_coefficient(fx.delay_feedback);
  cfg.delay_level = gs_effect_level(fx.delay_level);
  cfg.delay_level_center = gs_effect_level(fx.delay_level_center);
  cfg.delay_level_left = gs_effect_level(fx.delay_level_left);
  cfg.delay_level_right = gs_effect_level(fx.delay_level_right);
  cfg.delay_pre_lpf_hz = gs_pre_lpf_cutoff_hz(fx.delay_pre_lpf);
  cfg.delay_send_to_reverb = gs_effect_level(fx.delay_send_to_reverb);
  return cfg;
}

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

void GsEffectBus::set_config(const GsEffectsConfig& config) noexcept {
  const bool enable_reverb = config_.enable_reverb;
  const bool enable_chorus = config_.enable_chorus;
  const bool enable_delay = config_.enable_delay;
  config_ = config;
  config_.enable_reverb = enable_reverb;
  config_.enable_chorus = enable_chorus;
  config_.enable_delay = enable_delay;
  // Automatable parameter ids, from each unit's header. Everything reached here
  // is a coefficient: nothing resizes a buffer or clears a delay line.
  const effects::reverb::DattorroReverbConfig rc = reverb_config(config_);
  reverb_.set_parameter(0, rc.decay);
  reverb_.set_parameter(1, rc.damping);
  const effects::modulation::ChorusConfig cc = chorus_config(config_);
  chorus_.set_parameter(0, cc.rate_hz);
  chorus_.set_parameter(1, cc.depth_ms);
  chorus_.set_parameter(2, cc.center_delay_ms);
  delay_.set_config(delay_config(config_));
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
    // Energy falls by `decay` per tank pass. Ring-out to -80 dB takes
    // n = ln(1e-4) / ln(decay) passes.
    const double decay = std::clamp(static_cast<double>(config_.reverb_decay), 0.05, 0.98);
    const double passes = std::log(1.0e-4) / std::log(decay);
    tail_s = std::max(tail_s, kTankPassSeconds * passes);
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
