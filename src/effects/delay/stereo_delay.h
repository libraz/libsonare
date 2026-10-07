#pragma once

/// @file stereo_delay.h
/// @brief Zero-latency stereo feedback delay.
///
/// `feedback` carries its sign into the loop, as the flanger's does: a negative
/// gain inverts every pass, so the echoes alternate in polarity and the comb the
/// loop leaves has its teeth halfway between the ones a positive gain puts there.
///
/// Taps 1 and 2 are the left and right lines, whose outputs also feed the loop.
/// Taps 3 and 4 are feed-forward reads of the mono input, placed by a pan.

#include <array>
#include <vector>

#include "effects/common/mix_law.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::delay {

/// How the feedback loop is routed between the two lines.
enum class StereoDelayCrossMode {
  kNormal,    ///< each line feeds itself, ping_pong sets the crossing.
  kPingPong,  ///< mono input into the left line only, feedback fully crossed.
  kCross,     ///< feedback fully crossed, inputs stay on their sides.
};
inline constexpr int kStereoDelayCrossModeCount = 3;

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
  /// Feed-forward taps 3 and 4 in ms, up to the 4 s the lines hold. Zero disables.
  float tap3_ms = 0.0f;
  float tap4_ms = 0.0f;
  /// Output level of each tap in dB; taps 1 and 2 scale only what is heard, not the loop.
  float tap1_level_db = 0.0f;
  float tap2_level_db = 0.0f;
  float tap3_level_db = 0.0f;
  float tap4_level_db = 0.0f;
  /// Position of taps 3 and 4, -1 left to +1 right, constant power (centre -3 dB per side).
  float tap3_pan = 0.0f;
  float tap4_pan = 0.0f;
  /// Polarity of the wet path per side.
  bool invert_l = false;
  bool invert_r = false;
  /// Sine modulation of every delay time. Depth zero is off; the right side and tap 4 lag by
  /// `mod_phase_deg`. A modulated time is clamped to [0, 4 s].
  float mod_rate_hz = 0.0f;
  float mod_depth_ms = 0.0f;
  float mod_phase_deg = 0.0f;
  /// Time constant of the slew that follows a delay-time change; zero keeps the 10 ms default.
  float glide_ms = 0.0f;
  StereoDelayCrossMode cross_mode = StereoDelayCrossMode::kNormal;
  common::MixLaw mix_law = common::MixLaw::kCrossfade;
  /// How the lines read between samples. Lagrange3 reads no closer than one
  /// sample behind the write head, so a loop delay of 0 ms becomes one sample.
  modulation::DelayInterpolation interpolation = modulation::DelayInterpolation::kLinear;
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
  //   2 = feedback (clamped to +-common::kMaxFeedback, the sign carried; smoothed in process())
  //   3 = ping_pong (clamped to [0, 1], smoothed in process())
  //   4 = dry_wet (clamped to [0, 1], smoothed in process())
  //   5 = damping_hz (corner in Hz, <= 0 bypasses; rebuilds one coefficient)
  //   6/7 = tap3_ms/tap4_ms, 8..11 = tap1..4_level_db, 12/13 = tap3/4_pan
  //   14/15 = invert_l/r, 16..18 = mod_rate_hz/depth_ms/phase_deg, 19 = glide_ms
  //   20 = cross_mode, 21 = mix_law, 22 = interpolation (0 linear, 1 Lagrange3)
  bool set_parameter_impl(unsigned int param_id, float value) override;
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

  /// Hands config_.interpolation to every line.
  void apply_interpolation() noexcept;

  StereoDelayConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<modulation::ModDelayLine, 2> delays_;
  /// Taps 3 and 4, each its own read of the mono input.
  std::array<modulation::ModDelayLine, 2> tap_delays_;
  std::array<float, 2> tap_samples_{{0.0f, 0.0f}};
  double mod_phase_ = 0.0;
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
