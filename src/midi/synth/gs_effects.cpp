#include "midi/synth/gs_effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

using sonare::constants::kDefaultDawSampleRate;

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
  dc.feedback = std::clamp(cfg.delay_feedback, -0.9f, 0.9f);
  dc.ping_pong = 0.0f;
  dc.dry_wet = 1.0f;
  return dc;
}

effects::delay::StereoDelayConfig reverb_delay_config(const GsEffectsConfig& cfg, bool panning) {
  effects::delay::StereoDelayConfig dc;
  // PREDELAY shifts only the input, never the loop period; DELAY TIME CENTER/FEEDBACK are not read.
  dc.delay_time_l_ms = std::max(1.0f, cfg.reverb_delay_time_ms);
  dc.delay_time_r_ms = dc.delay_time_l_ms;
  dc.feedback = std::clamp(cfg.reverb_delay_feedback, 0.0f, 0.95f);
  dc.dry_wet = 1.0f;
  dc.cross_mode = panning ? effects::delay::StereoDelayCrossMode::kPingPong
                          : effects::delay::StereoDelayCrossMode::kNormal;
  return dc;
}

float reverb_predelay_samples(float milliseconds, double sample_rate) noexcept {
  if (!(sample_rate > 0.0)) return 0.0f;
  const float samples = std::max(0.0f, milliseconds) * 0.001f * static_cast<float>(sample_rate);
  return std::clamp(samples, 0.0f, 4.0f * static_cast<float>(sample_rate));
}

double delay_feedback_magnitude(float feedback) noexcept {
  return std::fabs(std::clamp(static_cast<double>(feedback), -0.9, 0.9));
}

double delay_time_milliseconds(float delay_time_ms) noexcept {
  return std::max(1.0, static_cast<double>(delay_time_ms));
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
  cfg.reverb_character = fx.reverb_character;
  cfg.reverb_decay = tank_decay_from_rt60(gs_reverb_time_seconds(fx.reverb_time));
  cfg.reverb_damping = gs_reverb_character_damping(fx.reverb_character);
  cfg.reverb_level = gs_return_level(fx.reverb_level);
  cfg.reverb_predelay_ms = gs_reverb_predelay_ms(fx.reverb_predelay);
  cfg.reverb_delay_time_ms = gs_reverb_delay_time_ms_designed(fx.reverb_time);
  cfg.reverb_delay_feedback = gs_reverb_delay_feedback_coefficient(fx.reverb_delay_feedback);
  cfg.reverb_pre_lpf_hz = gs_pre_lpf_cutoff_hz(fx.reverb_pre_lpf);

  cfg.chorus_rate_hz = gs_chorus_rate_hz(fx.chorus_rate);
  cfg.chorus_depth_ms = gs_chorus_depth_ms(fx.chorus_depth);
  cfg.chorus_delay_ms = gs_chorus_delay_ms(fx.chorus_delay);
  cfg.chorus_feedback = gs_chorus_feedback_coefficient(fx.chorus_feedback);
  cfg.chorus_level = gs_return_level(fx.chorus_level);
  cfg.chorus_pre_lpf_hz = gs_pre_lpf_cutoff_hz(fx.chorus_pre_lpf);
  cfg.chorus_send_to_reverb = gs_effect_level(fx.chorus_send_to_reverb);
  cfg.chorus_send_to_delay = gs_effect_level(fx.chorus_send_to_delay);

  cfg.delay_time_ms = gs_delay_time_ms(fx.delay_time_center);
  cfg.delay_time_ratio_left = gs_delay_time_ratio_percent(fx.delay_time_ratio_left) / 100.0f;
  cfg.delay_time_ratio_right = gs_delay_time_ratio_percent(fx.delay_time_ratio_right) / 100.0f;
  cfg.delay_feedback = gs_delay_feedback_coefficient(fx.delay_feedback);
  cfg.delay_level = gs_return_level(fx.delay_level);
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
      reverb_delay_(reverb_delay_config(config, false)),
      reverb_panning_delay_(reverb_delay_config(config, true)),
      chorus_(chorus_config(config)),
      delay_(delay_config(config)) {
  reverb_delay_predelay_ms_ = config.reverb_predelay_ms;
  reverb_panning_delay_predelay_ms_ = config.reverb_predelay_ms;
  reset_delay_history();
  reset_reverb_history();
  reset_reverb_activation();
}

void GsEffectBus::prepare(double sample_rate) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  const int max_delay_samples = static_cast<int>(sample_rate_ * 4.0);
  for (int ch = 0; ch < 2; ++ch) {
    reverb_bus_[ch].assign(kBlockFrames, 0.0f);
    reverb_delay_bus_[ch].assign(kBlockFrames, 0.0f);
    reverb_panning_delay_bus_[ch].assign(kBlockFrames, 0.0f);
    chorus_bus_[ch].assign(kBlockFrames, 0.0f);
    delay_bus_[ch].assign(kBlockFrames, 0.0f);
  }
  for (auto& line : reverb_delay_predelay_) line.prepare(max_delay_samples);
  for (auto& line : reverb_panning_delay_predelay_) line.prepare(max_delay_samples);
  reverb_.prepare(sample_rate_, kBlockFrames);
  reverb_delay_.prepare(sample_rate_, kBlockFrames);
  reverb_panning_delay_.prepare(sample_rate_, kBlockFrames);
  chorus_.prepare(sample_rate_, kBlockFrames);
  delay_.prepare(sample_rate_, kBlockFrames);
  reset_delay_history();
  reset_reverb_history();
  reset_reverb_activation();
}

void GsEffectBus::set_config(const GsEffectsConfig& config) noexcept {
  const bool enable_reverb = config_.enable_reverb;
  const bool enable_chorus = config_.enable_chorus;
  const bool enable_delay = config_.enable_delay;
  config_ = config;
  config_.enable_reverb = enable_reverb;
  config_.enable_chorus = enable_chorus;
  config_.enable_delay = enable_delay;
  observe_delay_config(config_);
  observe_reverb_config(config_);
  activate_reverb_mode(config_.reverb_character);
  // Automatable parameter ids, from each unit's header. Everything reached here
  // is a coefficient: nothing resizes a buffer or clears a delay line.
  // Update only the selected unit; the others keep draining their tail on the old macro's values.
  if (!gs_reverb_character_is_delay(config_.reverb_character)) {
    const effects::reverb::DattorroReverbConfig rc = reverb_config(config_);
    reverb_.set_parameter(0, rc.decay);
    reverb_.set_parameter(1, rc.damping);
  } else if (config_.reverb_character == 6) {
    reverb_delay_.set_config(reverb_delay_config(config_, false));
    reverb_delay_predelay_ms_ = config_.reverb_predelay_ms;
  } else {
    reverb_panning_delay_.set_config(reverb_delay_config(config_, true));
    reverb_panning_delay_predelay_ms_ = config_.reverb_predelay_ms;
  }
  const effects::modulation::ChorusConfig cc = chorus_config(config_);
  chorus_.set_parameter(0, cc.rate_hz);
  chorus_.set_parameter(1, cc.depth_ms);
  chorus_.set_parameter(2, cc.center_delay_ms);
  delay_.set_config(delay_config(config_));
}

void GsEffectBus::reset() {
  reverb_.reset();
  reverb_delay_.reset();
  reverb_panning_delay_.reset();
  for (auto& line : reverb_delay_predelay_) line.reset();
  for (auto& line : reverb_panning_delay_predelay_) line.reset();
  chorus_.reset();
  delay_.reset();
  reset_delay_history();
  reset_reverb_history();
  reset_reverb_activation();
  begin_chunk();
}

void GsEffectBus::observe_delay_config(const GsEffectsConfig& config) noexcept {
  max_delay_feedback_abs_ =
      std::max(max_delay_feedback_abs_, delay_feedback_magnitude(config.delay_feedback));
  max_delay_time_ms_ = std::max(max_delay_time_ms_, delay_time_milliseconds(config.delay_time_ms));
}

void GsEffectBus::reset_delay_history() noexcept {
  max_delay_feedback_abs_ = delay_feedback_magnitude(config_.delay_feedback);
  max_delay_time_ms_ = delay_time_milliseconds(config_.delay_time_ms);
}

void GsEffectBus::observe_reverb_config(const GsEffectsConfig& config) noexcept {
  if (gs_reverb_character_is_delay(config.reverb_character)) {
    max_reverb_delay_feedback_ =
        std::max(max_reverb_delay_feedback_,
                 std::clamp(static_cast<double>(config.reverb_delay_feedback), 0.0, 0.95));
    max_reverb_delay_time_ms_ = std::max(
        max_reverb_delay_time_ms_, std::max(1.0, static_cast<double>(config.reverb_delay_time_ms)));
    max_reverb_predelay_ms_ = std::max(
        max_reverb_predelay_ms_, std::max(0.0, static_cast<double>(config.reverb_predelay_ms)));
  } else {
    has_reverb_decay_history_ = true;
    max_reverb_decay_ = std::max(max_reverb_decay_,
                                 std::clamp(static_cast<double>(config.reverb_decay), 0.0, 0.98));
  }
}

void GsEffectBus::reset_reverb_history() noexcept {
  max_reverb_delay_feedback_ = 0.0;
  max_reverb_delay_time_ms_ = 0.0;
  max_reverb_predelay_ms_ = 0.0;
  max_reverb_decay_ = 0.0;
  has_reverb_decay_history_ = false;
  observe_reverb_config(config_);
}

void GsEffectBus::activate_reverb_mode(uint8_t character) noexcept {
  if (!gs_reverb_character_is_delay(character)) {
    reverb_tank_used_ = true;
  } else if (character == 6) {
    reverb_delay_used_ = true;
  } else {
    reverb_panning_delay_used_ = true;
  }
}

void GsEffectBus::reset_reverb_activation() noexcept {
  reverb_tank_used_ = false;
  reverb_delay_used_ = false;
  reverb_panning_delay_used_ = false;
  activate_reverb_mode(config_.reverb_character);
}

void GsEffectBus::begin_chunk() noexcept {
  for (int ch = 0; ch < 2; ++ch) {
    std::memset(reverb_bus_[ch].data(), 0, sizeof(float) * reverb_bus_[ch].size());
    std::memset(reverb_delay_bus_[ch].data(), 0, sizeof(float) * reverb_delay_bus_[ch].size());
    std::memset(reverb_panning_delay_bus_[ch].data(), 0,
                sizeof(float) * reverb_panning_delay_bus_[ch].size());
    std::memset(chorus_bus_[ch].data(), 0, sizeof(float) * chorus_bus_[ch].size());
    std::memset(delay_bus_[ch].data(), 0, sizeof(float) * delay_bus_[ch].size());
  }
}

void GsEffectBus::render_returns(float* out_l, float* out_r, int n) noexcept {
  if (n <= 0) return;
  n = std::min(n, kBlockFrames);
  if (config_.enable_reverb) {
    const bool normal_reverb = !gs_reverb_character_is_delay(config_.reverb_character);
    const bool normal_delay = config_.reverb_character == 6;
    const bool panning_delay = !normal_delay && !normal_reverb;
    activate_reverb_mode(config_.reverb_character);

    // Only the selected unit gets the send; a once-selected inactive unit drains on a zeroed bus.
    for (int ch = 0; ch < 2; ++ch) {
      if (normal_delay || panning_delay) {
        std::memcpy((normal_delay ? reverb_delay_bus_[ch] : reverb_panning_delay_bus_[ch]).data(),
                    reverb_bus_[ch].data(), sizeof(float) * static_cast<size_t>(n));
      }
      if (normal_reverb) {
        std::memset(reverb_delay_bus_[ch].data(), 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(reverb_panning_delay_bus_[ch].data(), 0,
                    sizeof(float) * static_cast<size_t>(n));
      } else {
        std::memset(reverb_bus_[ch].data(), 0, sizeof(float) * static_cast<size_t>(n));
      }
    }
    float* reverb_bus[2] = {reverb_bus_[0].data(), reverb_bus_[1].data()};
    float* reverb_delay_bus[2] = {reverb_delay_bus_[0].data(), reverb_delay_bus_[1].data()};
    float* reverb_panning_delay_bus[2] = {reverb_panning_delay_bus_[0].data(),
                                          reverb_panning_delay_bus_[1].data()};
    if (reverb_tank_used_) reverb_.process(reverb_bus, 2, n);
    if (reverb_delay_used_) {
      const float predelay_samples =
          reverb_predelay_samples(reverb_delay_predelay_ms_, sample_rate_);
      for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < n; ++i) {
          reverb_delay_bus[ch][i] =
              reverb_delay_predelay_[ch].process(reverb_delay_bus[ch][i], predelay_samples);
        }
      }
      reverb_delay_.process(reverb_delay_bus, 2, n);
    }
    if (reverb_panning_delay_used_) {
      const float predelay_samples =
          reverb_predelay_samples(reverb_panning_delay_predelay_ms_, sample_rate_);
      for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < n; ++i) {
          reverb_panning_delay_bus[ch][i] = reverb_panning_delay_predelay_[ch].process(
              reverb_panning_delay_bus[ch][i], predelay_samples);
        }
      }
      reverb_panning_delay_.process(reverb_panning_delay_bus, 2, n);
    }
    // Each unit's LEVEL scales its return. A unit is linear, so this is the same
    // as scaling everything sent to it -- including the per-program ambience
    // weights of gm_fallback_sends, which is why unity has to be the reset value
    // and not full scale (gs_return_level).
    const float g = config_.reverb_level;
    for (int i = 0; i < n; ++i) {
      out_l[i] += (reverb_bus[0][i] + reverb_delay_bus[0][i] + reverb_panning_delay_bus[0][i]) * g;
      out_r[i] += (reverb_bus[1][i] + reverb_delay_bus[1][i] + reverb_panning_delay_bus[1][i]) * g;
    }
  }
  if (config_.enable_chorus) {
    float* bus[2] = {chorus_bus_[0].data(), chorus_bus_[1].data()};
    chorus_.process(bus, 2, n);
    const float g = config_.chorus_level;
    for (int i = 0; i < n; ++i) {
      out_l[i] += bus[0][i] * g;
      out_r[i] += bus[1][i] * g;
    }
  }
  if (config_.enable_delay) {
    float* bus[2] = {delay_bus_[0].data(), delay_bus_[1].data()};
    delay_.process(bus, 2, n);
    const float g = config_.delay_level;
    for (int i = 0; i < n; ++i) {
      out_l[i] += bus[0][i] * g;
      out_r[i] += bus[1][i] * g;
    }
  }
}

int64_t GsEffectBus::tail_samples(double sample_rate) const noexcept {
  if (!(sample_rate > 0.0)) return 0;
  double tail_s = 0.0;
  if (config_.enable_reverb) {
    // Energy falls by `decay` per tank pass. Ring-out to -80 dB takes
    // n = ln(1e-4) / ln(decay) passes.
    const double decay = std::clamp(max_reverb_decay_, 0.05, 0.98);
    if (has_reverb_decay_history_) {
      const double passes = std::log(1.0e-4) / std::log(decay);
      tail_s = std::max(tail_s, kTankPassSeconds * passes);
    }
  }
  if (config_.enable_reverb && max_reverb_delay_time_ms_ > 0.0) {
    const double fb = max_reverb_delay_feedback_;
    const double time_s = max_reverb_delay_time_ms_ * 0.001;
    const double predelay_s = max_reverb_predelay_ms_ * 0.001;
    const double repeats = fb > 0.0 ? std::log(1.0e-4) / std::log(fb) : 0.0;
    const double loop_s = time_s + 1.0 / sample_rate;
    // PREDELAY delays the first echo but does not lengthen a feedback lap.
    tail_s = std::max(tail_s, predelay_s + time_s + repeats * loop_s);
  }
  if (config_.enable_delay) {
    const double fb = max_delay_feedback_abs_;
    const double time_s = max_delay_time_ms_ * 0.001;
    const double repeats = fb > 0.0 ? std::log(1.0e-4) / std::log(fb) : 0.0;
    // Each lap adds one frame (the feedback cell); bound the last echo above -80 dB.
    const double loop_s = time_s + 1.0 / sample_rate;
    tail_s = std::max(tail_s, time_s + repeats * loop_s);
  }
  if (config_.enable_chorus) {
    tail_s = std::max(tail_s, 0.1);  // modulated delay line ring-out
  }
  return static_cast<int64_t>(std::ceil(tail_s * sample_rate));
}

}  // namespace sonare::midi::synth
