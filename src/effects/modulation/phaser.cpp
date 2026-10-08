#include "effects/modulation/phaser.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

using common::kMaxFeedback;
using sonare::constants::kPi;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

namespace {

// Corner of the single high-pass pole inside the feedback loop. The measurement
// places it below the lowest band it could read, so this is the lowest corner
// consistent with that reading rather than a resolved value.
constexpr double kLoopHighpassHz = 20.0;

// A cascade of allpass sections passes every frequency at unit gain, so a loop
// closed around it is stable while its gain stays under one (kMaxFeedback).

double effective_sample_rate(double sample_rate) noexcept {
  return sample_rate > 0.0 && std::isfinite(sample_rate) ? sample_rate : 48000.0;
}

float max_sweep_hz(double sample_rate) noexcept {
  return static_cast<float>(effective_sample_rate(sample_rate) * 0.49);
}

}  // namespace

Phaser::Phaser(PhaserConfig config) : config_(config) {}

void Phaser::prepare(double sample_rate, int) {
  sample_rate_ = effective_sample_rate(sample_rate);
  const int stages = std::clamp(config_.stages, 1, 12);
  for (int ch = 0; ch < 2; ++ch) {
    x1_[static_cast<size_t>(ch)].assign(static_cast<size_t>(stages), 0.0f);
    y1_[static_cast<size_t>(ch)].assign(static_cast<size_t>(stages), 0.0f);
  }
  lfos_[0].prepare(sample_rate_);
  lfos_[1].prepare(sample_rate_);
  lfos_[0].set_rate_hz(config_.rate_hz);
  lfos_[1].set_rate_hz(config_.rate_hz);
  // exp(-2*pi*fc/sr): evaluated here so the corner stays a frequency in hertz.
  loop_highpass_pole_ = static_cast<float>(std::exp(-kTwoPiD * kLoopHighpassHz / sample_rate_));
  reset();
}

void Phaser::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const bool stereo = right != left;
  // Block-rate dry/wet + modulation depth: smoothed across blocks by the engine
  // parameter slot smoother, not per-sample (see Chorus::process for the rationale).
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  // A sum holds the dry signal at unity and lets the pair reach twice the input
  // where they arrive in phase; a crossfade splits one unit between them.
  const float dry = config_.mix_mode == PhaserMixMode::kDrySum ? 1.0f : 1.0f - wet;
  const float feedback = std::clamp(config_.feedback, -kMaxFeedback, kMaxFeedback);
  // Top of the sweep for this block; a full depth is max_hz itself, not its pow() image.
  const float depth = std::clamp(config_.depth, 0.0f, 1.0f);
  // Bounds stay in hertz across rate changes; only this block's copies meet Nyquist.
  const float sweep_limit = max_sweep_hz(sample_rate_);
  const float effective_min_hz = std::clamp(config_.min_hz, 1.0f, sweep_limit);
  const float effective_max_hz = std::clamp(config_.max_hz, 1.0f, sweep_limit);
  const float min_hz = std::min(effective_min_hz, effective_max_hz);
  const float max_hz = std::max(effective_min_hz, effective_max_hz);
  const float top_hz = depth >= 1.0f ? max_hz : min_hz * std::pow(max_hz / min_hz, depth);
  for (int i = 0; i < num_samples; ++i) {
    const float coeff_l = sweep_coeff(lfos_[0].process(), min_hz, top_hz);
    const float in_l = left[i];
    left[i] = dry * in_l + wet * process_channel(in_l, 0, coeff_l, feedback);
    if (stereo) {
      // Only advance the channel-1 LFO and allpass state for genuine stereo
      // input so a mono buffer is not written twice and channel-1 state is left
      // untouched. The quarter-cycle offset between the two oscillators is what
      // keeps the notches from tracking each other across the pair.
      const float coeff_r = sweep_coeff(lfos_[1].process(), min_hz, top_hz);
      const float in_r = right[i];
      right[i] = dry * in_r + wet * process_channel(in_r, 1, coeff_r, feedback);
    }
  }
  discard_non_finite();
}

void Phaser::discard_non_finite() noexcept {
  bool discarded = false;
  for (size_t ch = 0; ch < y1_.size(); ++ch) {
    auto& x = x1_[ch];
    auto& z = y1_[ch];
    // A stage's input tap and output tap are one section, so either one being
    // poisoned returns both -- and the loop's cells recirculate through those
    // same sections, so they are part of the same failure and return with them.
    if (discard_run_if_non_finite(z.begin(), z.end(), 0.0f) ||
        discard_run_if_non_finite(x.begin(), x.end(), 0.0f) ||
        discard_group_if_non_finite(loop_return_[ch], loop_highpass_in_[ch],
                                    loop_highpass_out_[ch])) {
      std::fill(x.begin(), x.end(), 0.0f);
      std::fill(z.begin(), z.end(), 0.0f);
      loop_return_[ch] = 0.0f;
      loop_highpass_in_[ch] = 0.0f;
      loop_highpass_out_[ch] = 0.0f;
      discarded = true;
    }
  }
  // Accumulated rather than bumped per channel: the channel count is the
  // caller's buffer, not their unit of work.
  if (discarded) note_non_finite_discard();
}

bool Phaser::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.rate_hz = std::max(0.0f, value);
      // Updates the LFO increment in place; preserves oscillator phase, so the
      // L/R offset survives a rate change.
      lfos_[0].set_rate_hz(config_.rate_hz);
      lfos_[1].set_rate_hz(config_.rate_hz);
      return true;
    case 1:
      // Stored in hertz as written; process() bounds and orders the pair per block.
      config_.min_hz = value;
      return true;
    case 2:
      config_.max_hz = value;
      return true;
    case 3:
      config_.dry_wet = value;
      return true;
    case 4:
      // process() clamps feedback to +-kMaxFeedback; store the raw target.
      config_.feedback = value;
      return true;
    case 5:
      config_.depth = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

bool Phaser::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every automatable id performs an in-place scalar/coefficient update (LFO
  // rate, sweep bounds, dry/wet, loop gain); none allocates or resets audio
  // state. Unknown ids are rejected by set_parameter.
  return param_id <= 5;
}

std::vector<rt::ParamDescriptor> Phaser::parameter_descriptors() const {
  return {{"rateHz", 0}, {"minHz", 1}, {"maxHz", 2}, {"dryWet", 3}, {"feedback", 4}, {"depth", 5}};
}

int Phaser::tail_samples() const noexcept {
  if (!(std::clamp(config_.dry_wet, 0.0f, 1.0f) > 0.0f)) return 0;
  // The sweep's lowest corner gives the slowest allpass pole; the cascade then
  // recirculates through the one-sample loop and its 20 Hz high-pass.
  const float sweep_limit = max_sweep_hz(sample_rate_);
  const double low_hz = std::min(std::clamp(config_.min_hz, 1.0f, sweep_limit),
                                 std::clamp(config_.max_hz, 1.0f, sweep_limit));
  const double t = std::tan(kPiD * low_hz / sample_rate_);
  const double pole = (1.0 - t) / (1.0 + t);
  rt::TailBudget cascade;
  for (int stage = 0; stage < std::clamp(config_.stages, 1, 12); ++stage) cascade.decay(pole);
  rt::TailBudget tail = cascade;
  tail.recirculation(cascade.samples() + 1.0,
                     std::clamp(config_.feedback, -kMaxFeedback, kMaxFeedback))
      .decay(loop_highpass_pole_);
  return tail.samples();
}

void Phaser::reset() {
  for (auto& state : x1_) {
    std::fill(state.begin(), state.end(), 0.0f);
  }
  for (auto& state : y1_) {
    std::fill(state.begin(), state.end(), 0.0f);
  }
  loop_return_.fill(0.0f);
  loop_highpass_in_.fill(0.0f);
  loop_highpass_out_.fill(0.0f);
  lfos_[0].reset(0.0);
  lfos_[1].reset(0.25);
}

float Phaser::sweep_coeff(float lfo_value, float min_hz, float top_hz) const noexcept {
  const float sweep = 0.5f + 0.5f * lfo_value;
  const float freq = min_hz + (top_hz - min_hz) * sweep;
  const float t = std::tan(kPi * freq / static_cast<float>(sample_rate_));
  return (1.0f - t) / (1.0f + t);
}

float Phaser::process_channel(float input, int channel, float coeff, float feedback) {
  const size_t ch = static_cast<size_t>(channel);
  // The return is a sample late: a part computing one sample at a time has the
  // previous cascade output and not the one it is in the middle of producing.
  float y = input + feedback * loop_return_[ch];
  auto& x = x1_[ch];
  auto& z = y1_[ch];
  for (size_t stage = 0; stage < x.size(); ++stage) {
    const float out = -coeff * y + x[stage] + coeff * z[stage];
    x[stage] = y;
    z[stage] = out;
    y = out;
  }
  // One high-pass pole, in the return path and nowhere else in the chain: the
  // bottom of the band drops away as the loop closes and not when it is open.
  const float blocked = loop_highpass_pole_ * (loop_highpass_out_[ch] + y - loop_highpass_in_[ch]);
  loop_highpass_in_[ch] = y;
  loop_highpass_out_[ch] = blocked;
  loop_return_[ch] = blocked;
  return y;
}

}  // namespace sonare::effects::modulation
