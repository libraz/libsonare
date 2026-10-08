#include "effects/delay/stereo_delay.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/non_finite_state.h"

namespace sonare::effects::delay {

using common::kLevelFloorDb;
using common::kMaxFeedback;
using constants::kPiD;
using constants::kTwoPiD;

namespace {

constexpr float kDelaySmoothingTimeSeconds = 0.010f;
constexpr float kMaxDelayMs = 4000.0f;
constexpr float kMaxModRateHz = 100.0f;
constexpr float kMaxGlideMs = 10000.0f;
constexpr float kMaxLevelDb = 40.0f;

float clamp_feedback(float feedback) noexcept {
  return std::clamp(feedback, -kMaxFeedback, kMaxFeedback);
}

float config_delay_samples(float delay_ms, double sample_rate) noexcept {
  return std::clamp(delay_ms, 0.0f, kMaxDelayMs) * 0.001f * static_cast<float>(sample_rate);
}

/// A non-finite value falls back to `fallback`; the rest is clamped.
float finite_clamp(float value, float lo, float hi, float fallback) noexcept {
  return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

/// Left and right gains of a constant-power pan, -1 left to +1 right.
std::array<float, 2> pan_gains(float pan) noexcept {
  const double angle = (static_cast<double>(std::clamp(pan, -1.0f, 1.0f)) + 1.0) * kPiD * 0.25;
  return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

StereoDelayConfig sanitize_config(StereoDelayConfig config) noexcept {
  config.delay_time_l_ms = std::clamp(config.delay_time_l_ms, 0.0f, kMaxDelayMs);
  config.delay_time_r_ms = std::clamp(config.delay_time_r_ms, 0.0f, kMaxDelayMs);
  config.feedback = clamp_feedback(config.feedback);
  config.ping_pong = std::clamp(config.ping_pong, 0.0f, 1.0f);
  config.dry_wet = std::clamp(config.dry_wet, 0.0f, 1.0f);
  // std::clamp leaves NaN intact and an infinite corner has no pole, so both
  // fall back to the bypass rather than reaching the exp() below.
  if (!std::isfinite(config.damping_hz) || config.damping_hz <= 0.0f) {
    config.damping_hz = 0.0f;
  }
  config.tap3_ms = finite_clamp(config.tap3_ms, 0.0f, kMaxDelayMs, 0.0f);
  config.tap4_ms = finite_clamp(config.tap4_ms, 0.0f, kMaxDelayMs, 0.0f);
  config.tap1_level_db = finite_clamp(config.tap1_level_db, kLevelFloorDb, kMaxLevelDb, 0.0f);
  config.tap2_level_db = finite_clamp(config.tap2_level_db, kLevelFloorDb, kMaxLevelDb, 0.0f);
  config.tap3_level_db = finite_clamp(config.tap3_level_db, kLevelFloorDb, kMaxLevelDb, 0.0f);
  config.tap4_level_db = finite_clamp(config.tap4_level_db, kLevelFloorDb, kMaxLevelDb, 0.0f);
  config.tap3_pan = finite_clamp(config.tap3_pan, -1.0f, 1.0f, 0.0f);
  config.tap4_pan = finite_clamp(config.tap4_pan, -1.0f, 1.0f, 0.0f);
  config.mod_rate_hz = finite_clamp(config.mod_rate_hz, 0.0f, kMaxModRateHz, 0.0f);
  config.mod_depth_ms = finite_clamp(config.mod_depth_ms, 0.0f, kMaxDelayMs, 0.0f);
  config.mod_phase_deg = finite_clamp(config.mod_phase_deg, 0.0f, 360.0f, 0.0f);
  config.glide_ms = finite_clamp(config.glide_ms, 0.0f, kMaxGlideMs, 0.0f);
  return config;
}

}  // namespace

StereoDelay::StereoDelay(StereoDelayConfig config) : config_(sanitize_config(config)) {
  update_damping();
}

void StereoDelay::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const int max_delay = static_cast<int>(sample_rate_ * 4.0);
  for (auto& delay : delays_) {
    delay.prepare(max_delay);
  }
  for (auto& delay : tap_delays_) {
    delay.prepare(max_delay);
  }
  update_damping();
  apply_interpolation();
  reset();
}

void StereoDelay::apply_interpolation() noexcept {
  for (auto& delay : delays_) delay.set_interpolation(config_.interpolation);
  for (auto& delay : tap_delays_) delay.set_interpolation(config_.interpolation);
}

void StereoDelay::update_damping() noexcept {
  if (config_.damping_hz <= 0.0f || sample_rate_ <= 0.0) {
    damping_gain_ = 0.0f;
    return;
  }
  const double pole = std::exp(-kTwoPiD * config_.damping_hz / sample_rate_);
  damping_gain_ = static_cast<float>(1.0 - pole);
}

void StereoDelay::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float target_wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float target_feedback = clamp_feedback(config_.feedback);
  const float target_ping_pong = std::clamp(config_.ping_pong, 0.0f, 1.0f);
  const std::array<float, 2> target_delay_samples{
      config_delay_samples(config_.delay_time_l_ms, sample_rate_),
      config_delay_samples(config_.delay_time_r_ms, sample_rate_)};
  const std::array<float, 2> target_tap_samples{
      config_delay_samples(config_.tap3_ms, sample_rate_),
      config_delay_samples(config_.tap4_ms, sample_rate_)};
  const float smoothing_coeff = std::clamp(
      1.0f / std::max(1.0f, static_cast<float>(sample_rate_) * kDelaySmoothingTimeSeconds), 0.0f,
      1.0f);
  // The slew of a delay time follows the glide when one is set, and the smoothing time otherwise.
  const float delay_coeff =
      config_.glide_ms > 0.0f
          ? std::clamp(
                1.0f / std::max(1.0f, static_cast<float>(sample_rate_) * 0.001f * config_.glide_ms),
                0.0f, 1.0f)
          : smoothing_coeff;
  const bool tap_on[2] = {config_.tap3_ms > 0.0f, config_.tap4_ms > 0.0f};
  const float tap_level[2] = {db_to_linear(config_.tap3_level_db),
                              db_to_linear(config_.tap4_level_db)};
  const std::array<float, 2> tap_pan[2] = {pan_gains(config_.tap3_pan),
                                           pan_gains(config_.tap4_pan)};
  const float level_l = db_to_linear(config_.tap1_level_db);
  const float level_r = db_to_linear(config_.tap2_level_db);
  const bool invert_l = config_.invert_l;
  const bool invert_r = config_.invert_r;
  const StereoDelayCrossMode cross_mode = config_.cross_mode;
  const common::MixLaw mix_law = config_.mix_law;
  const bool modulated = config_.mod_depth_ms > 0.0f;
  const double phase_step = static_cast<double>(config_.mod_rate_hz) / sample_rate_;
  const double phase_offset = static_cast<double>(config_.mod_phase_deg) / 360.0;
  const float depth_samples = config_.mod_depth_ms * 0.001f * static_cast<float>(sample_rate_);

  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // Damping re-enters the loop from the signal circulating now, never from the
  // sample it held when it was switched off.
  const bool damped =
      damping_gate_.admit(damping_gain_ > 0.0f, [this] { damping_state_ = feedback_state_; });
  for (int i = 0; i < num_samples; ++i) {
    delay_samples_[0] += (target_delay_samples[0] - delay_samples_[0]) * delay_coeff;
    delay_samples_[1] += (target_delay_samples[1] - delay_samples_[1]) * delay_coeff;
    tap_samples_[0] += (target_tap_samples[0] - tap_samples_[0]) * delay_coeff;
    tap_samples_[1] += (target_tap_samples[1] - tap_samples_[1]) * delay_coeff;
    smoothed_feedback_ += (target_feedback - smoothed_feedback_) * smoothing_coeff;
    smoothed_dry_wet_ += (target_wet - smoothed_dry_wet_) * smoothing_coeff;
    smoothed_ping_pong_ += (target_ping_pong - smoothed_ping_pong_) * smoothing_coeff;
    // Modulated times may pass the allocated line; ModDelayLine clamps the read to [0, 4 s].
    float read_l = delay_samples_[0];
    float read_r = delay_samples_[1];
    float read_tap[2] = {tap_samples_[0], tap_samples_[1]};
    if (modulated) {
      const double phase_r = mod_phase_ - phase_offset;
      const float mod_l = depth_samples * static_cast<float>(std::sin(kTwoPiD * mod_phase_));
      const float mod_r = depth_samples * static_cast<float>(std::sin(kTwoPiD * phase_r));
      read_l += mod_l;
      read_r += mod_r;
      read_tap[0] += mod_l;
      read_tap[1] += mod_r;
    }
    mod_phase_ += phase_step;
    mod_phase_ -= std::floor(mod_phase_);
    const float wet = smoothed_dry_wet_;
    const common::MixGains mix = common::mix_gains(mix_law, wet);
    const float dry = mix.dry;
    const float wet_gain = mix.wet;
    // Ping-pong sends the mono input into the left line alone; cross keeps the inputs where they
    // are. Both cross the feedback completely.
    const float ping_pong =
        cross_mode != StereoDelayCrossMode::kNormal ? 1.0f : smoothed_ping_pong_;
    const float in_l = left[i];
    const float in_r = right[i];
    const float mid = stereo ? 0.5f * (in_l + in_r) : in_l;
    const float src_l = cross_mode == StereoDelayCrossMode::kPingPong ? mid : in_l;
    const float src_r = cross_mode == StereoDelayCrossMode::kPingPong ? 0.0f : in_r;
    const float feed_l = src_l + smoothed_feedback_ * ((1.0f - ping_pong) * feedback_state_[0] +
                                                       ping_pong * feedback_state_[1]);
    const float feed_r = src_r + smoothed_feedback_ * ((1.0f - ping_pong) * feedback_state_[1] +
                                                       ping_pong * feedback_state_[0]);
    float delayed_l = delays_[0].process(feed_l, read_l);
    float delayed_r = delays_[1].process(feed_r, read_r);
    if (damped) {
      // One multiply and one state, with no zero at Nyquist, sitting inside the
      // recirculation so every pass takes one helping of it. Which side of the
      // line it sits on is not observable once round.
      damping_state_[0] += damping_gain_ * (delayed_l - damping_state_[0]);
      damping_state_[1] += damping_gain_ * (delayed_r - damping_state_[1]);
      delayed_l = damping_state_[0];
      delayed_r = damping_state_[1];
    }
    feedback_state_ = {delayed_l, delayed_r};
    feedback_non_finite_ |= !std::isfinite(delayed_l) || !std::isfinite(delayed_r);
    float heard_l = delayed_l * level_l;
    float heard_r = delayed_r * level_r;
    for (int t = 0; t < 2; ++t) {
      // A disabled tap still takes its write so the line stays time-coherent.
      const float tap = tap_delays_[t].process(mid, read_tap[t]);
      feedback_non_finite_ |= !std::isfinite(tap);
      if (!tap_on[t]) continue;
      const float heard = tap * tap_level[t];
      heard_l += heard * tap_pan[t][0];
      heard_r += heard * tap_pan[t][1];
    }
    if (invert_l) heard_l = -heard_l;
    if (invert_r) heard_r = -heard_r;
    if (stereo) {
      left[i] = dry * in_l + wet_gain * heard_l;
      right[i] = dry * in_r + wet_gain * heard_r;
    } else {
      // Mono: collapse the two delay taps into the single output buffer so it
      // is not written twice with different values.
      left[i] = dry * in_l + wet_gain * 0.5f * (heard_l + heard_r);
    }
  }
  discard_non_finite();
}

void StereoDelay::discard_non_finite() noexcept {
  bool discarded = false;
  if (feedback_non_finite_) {
    feedback_non_finite_ = false;
    static_cast<void>(
        discard_run_if_non_finite(feedback_state_.begin(), feedback_state_.end(), 0.0f));
    // The damping cell sits in the same loop, so a poisoned one would feed the
    // recirculation straight back out again.
    static_cast<void>(
        discard_run_if_non_finite(damping_state_.begin(), damping_state_.end(), 0.0f));
    // Both lines are fed by the feedback cells that read them, so the poison
    // recirculates instead of flowing out. O(line), recovery only.
    for (auto& delay : delays_) delay.reset();
    for (auto& delay : tap_delays_) delay.reset();
    // The reset is itself a discard: the cells hold the block's last sample and
    // are often finite again while the lines still carried the poison.
    discarded = true;
  }
  // A smoother rests at its target, not at zero: zero would mute the mix and
  // drop the feedback for a smoothing time nobody asked for.
  discarded |= discard_if_non_finite(smoothed_feedback_, clamp_feedback(config_.feedback));
  discarded |= discard_if_non_finite(smoothed_dry_wet_, std::clamp(config_.dry_wet, 0.0f, 1.0f));
  discarded |=
      discard_if_non_finite(smoothed_ping_pong_, std::clamp(config_.ping_pong, 0.0f, 1.0f));
  if (discarded) note_non_finite_discard();
}

int StereoDelay::tail_samples() const noexcept {
  if (std::clamp(config_.dry_wet, 0.0f, 1.0f) <= 0.0f) return 0;
  const double to_samples = 0.001 * sample_rate_;
  const double line =
      std::clamp(std::max(config_.delay_time_l_ms, config_.delay_time_r_ms) + config_.mod_depth_ms,
                 0.0f, kMaxDelayMs) *
          to_samples +
      modulation::kDelayReadStencilSamples;
  // Each pass reads the longer line, then the one-sample feedback cell and the damping ring.
  rt::TailBudget damping;
  if (damping_gain_ > 0.0f) damping.decay(1.0 - damping_gain_);
  rt::TailBudget loop;
  loop.delay(line).recirculation(line + 1.0 + damping.samples(), clamp_feedback(config_.feedback));
  // The feed-forward taps ring once, at their own time.
  rt::TailBudget taps;
  taps.delay(std::clamp(std::max(config_.tap3_ms, config_.tap4_ms) + config_.mod_depth_ms, 0.0f,
                        kMaxDelayMs) *
                 to_samples +
             modulation::kDelayReadStencilSamples);
  return loop.alongside(taps).samples();
}

void StereoDelay::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  for (auto& delay : tap_delays_) {
    delay.reset();
  }
  mod_phase_ = 0.0;
  tap_samples_ = {config_delay_samples(config_.tap3_ms, sample_rate_),
                  config_delay_samples(config_.tap4_ms, sample_rate_)};
  delay_samples_ = {config_delay_samples(config_.delay_time_l_ms, sample_rate_),
                    config_delay_samples(config_.delay_time_r_ms, sample_rate_)};
  feedback_state_ = {0.0f, 0.0f};
  damping_state_ = {0.0f, 0.0f};
  feedback_non_finite_ = false;
  smoothed_feedback_ = clamp_feedback(config_.feedback);
  smoothed_dry_wet_ = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  smoothed_ping_pong_ = std::clamp(config_.ping_pong, 0.0f, 1.0f);
}

void StereoDelay::set_config(const StereoDelayConfig& config) noexcept {
  // Times, feedback and mix are smoothed per sample; damping and interpolation
  // are refreshed in place, so the delay lines keep their contents.
  rt::apply_config_diff(config_, sanitize_config(config),
                        [this](const rt::ConfigDiff<StereoDelayConfig>& diff) {
                          if (diff.changed(&StereoDelayConfig::damping_hz)) update_damping();
                          if (diff.changed(&StereoDelayConfig::interpolation)) {
                            apply_interpolation();
                          }
                        });
}

bool StereoDelay::set_parameter_impl(unsigned int param_id, float value) {
  // Reject before the clamps below: std::clamp leaves NaN intact and the delay
  // ids feed the fractional read index (see delay_param_acceptable).
  if (!modulation::delay_param_acceptable(value)) return false;
  switch (param_id) {
    case 0:
      // process() smooths delay-time automation before it reaches the
      // fractional read taps; the lines are sized for up to 4 s.
      config_.delay_time_l_ms = std::clamp(value, 0.0f, kMaxDelayMs);
      return true;
    case 1:
      config_.delay_time_r_ms = std::clamp(value, 0.0f, kMaxDelayMs);
      return true;
    case 2:
      config_.feedback = clamp_feedback(value);
      return true;
    case 3:
      // process() clamps ping_pong to [0, 1] and smooths it; store the raw
      // target.
      config_.ping_pong = value;
      return true;
    case 4:
      config_.dry_wet = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 5:
      // The corner is stored and the pole rebuilt here, the fourth live path
      // into it. A non-positive corner has to reach the branch that skips the
      // filter, not a gain of one, which is not bit-identical to a bypass.
      config_.damping_hz = value > 0.0f ? value : 0.0f;
      update_damping();
      return true;
    case 6:
      config_.tap3_ms = std::clamp(value, 0.0f, kMaxDelayMs);
      return true;
    case 7:
      config_.tap4_ms = std::clamp(value, 0.0f, kMaxDelayMs);
      return true;
    case 8:
      config_.tap1_level_db = std::clamp(value, kLevelFloorDb, kMaxLevelDb);
      return true;
    case 9:
      config_.tap2_level_db = std::clamp(value, kLevelFloorDb, kMaxLevelDb);
      return true;
    case 10:
      config_.tap3_level_db = std::clamp(value, kLevelFloorDb, kMaxLevelDb);
      return true;
    case 11:
      config_.tap4_level_db = std::clamp(value, kLevelFloorDb, kMaxLevelDb);
      return true;
    case 12:
      config_.tap3_pan = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 13:
      config_.tap4_pan = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 14:
    case 15:
      // A switch takes exactly 0 or 1.
      if (value != 0.0f && value != 1.0f) return false;
      (param_id == 14 ? config_.invert_l : config_.invert_r) = value == 1.0f;
      return true;
    case 16:
      config_.mod_rate_hz = std::clamp(value, 0.0f, kMaxModRateHz);
      return true;
    case 17:
      config_.mod_depth_ms = std::clamp(value, 0.0f, kMaxDelayMs);
      return true;
    case 18:
      config_.mod_phase_deg = std::clamp(value, 0.0f, 360.0f);
      return true;
    case 19:
      config_.glide_ms = std::clamp(value, 0.0f, kMaxGlideMs);
      return true;
    case 20:
      // An unnamed mode is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kStereoDelayCrossModeCount)) {
        return false;
      }
      config_.cross_mode = static_cast<StereoDelayCrossMode>(static_cast<int>(value));
      return true;
    case 21:
      return common::mix_law_from_value(value, &config_.mix_law);
    case 22:
      if (!modulation::delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<modulation::DelayInterpolation>(static_cast<int>(value));
      apply_interpolation();
      return true;
    default:
      return false;
  }
}

bool StereoDelay::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar update; the delay lines are
  // sized for up to kMaxDelayMs at prepare() and delay-time automation is
  // smoothed in process(), so no id allocates or resets audio state. The damping
  // corner recomputes one coefficient and leaves its cell where it stood, so it
  // is in-place too. Unknown ids are rejected by set_parameter.
  return param_id <= 22;
}

std::vector<rt::ParamDescriptor> StereoDelay::parameter_descriptors() const {
  return {{"delayTimeLMs", 0}, {"delayTimeRMs", 1}, {"feedback", 2},      {"pingPong", 3},
          {"dryWet", 4},       {"dampingHz", 5},    {"tap3Ms", 6},        {"tap4Ms", 7},
          {"tap1LevelDb", 8},  {"tap2LevelDb", 9},  {"tap3LevelDb", 10},  {"tap4LevelDb", 11},
          {"tap3Pan", 12},     {"tap4Pan", 13},     {"invertL", 14},      {"invertR", 15},
          {"modRateHz", 16},   {"modDepthMs", 17},  {"modPhaseDeg", 18},  {"glideMs", 19},
          {"crossMode", 20},   {"mixLaw", 21},      {"interpolation", 22}};
}

}  // namespace sonare::effects::delay
