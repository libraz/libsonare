#pragma once

/// @file stereo_delay.h
/// @brief Zero-latency stereo feedback delay.
///
/// `feedback` carries its sign into the loop, as the flanger's does: a negative
/// gain inverts every pass, so the echoes alternate in polarity and the comb the
/// loop leaves has its teeth halfway between the ones a positive gain puts there.

#include <array>
#include <vector>

#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::delay {

struct StereoDelayConfig {
  float delay_time_l_ms = 250.0f;
  float delay_time_r_ms = 250.0f;
  float feedback = 0.25f;
  float ping_pong = 0.0f;
  float dry_wet = 0.5f;
  /// Corner of the one-pole damping inside the feedback loop, in Hz. Zero (the
  /// default) bypasses it, which is both today's behaviour and the state the
  /// parameter's printed range carries beside its span.
  float damping_hz = 0.0f;
};

class StereoDelay : public rt::ProcessorBase {
 public:
  explicit StereoDelay(StereoDelayConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override;

  void set_config(const StereoDelayConfig& config) noexcept;
  const StereoDelayConfig& config() const noexcept { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = delay_time_l_ms
  //   1 = delay_time_r_ms
  //   2 = feedback (clamped to [-0.95, 0.95], the sign carried; smoothed in process())
  //   3 = ping_pong (clamped to [0, 1], smoothed in process())
  //   4 = dry_wet (clamped to [0, 1], smoothed in process())
  //   5 = damping_hz (corner in Hz, <= 0 bypasses; rebuilds one coefficient)
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Returns the feedback path and the parameter smoothers to rest once a
  /// non-finite value has reached them, once per block (see
  /// util/non_finite_state.h).
  void discard_non_finite() noexcept;

  /// Derives the damping pole from config_.damping_hz and the current sample
  /// rate. The corner is what is stored; the pole is non-linear in the rate and
  /// so is rebuilt rather than scaled whenever either changes.
  void update_damping() noexcept;

  StereoDelayConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<modulation::ModDelayLine, 2> delays_;
  std::array<float, 2> delay_samples_{{0.0f, 0.0f}};
  std::array<float, 2> feedback_state_{{0.0f, 0.0f}};
  /// Set by process() when a delay tap came back non-finite, cleared by
  /// discard_non_finite(). The poison is resident in the line rather than in a
  /// cell -- a tap reads it for a sample or two per lap and the cell is finite
  /// again by the end of the block -- so the block latches it as it passes.
  bool feedback_non_finite_ = false;
  /// The damping's single multiply, one minus its pole. Zero means the filter
  /// is out: a gain of one would read as a pass-through but is not bit-exact,
  /// so the bypass is a branch rather than a value.
  float damping_gain_ = 0.0f;
  std::array<float, 2> damping_state_{{0.0f, 0.0f}};
  float smoothed_feedback_ = 0.0f;
  float smoothed_dry_wet_ = 0.5f;
  float smoothed_ping_pong_ = 0.0f;
};

}  // namespace sonare::effects::delay
