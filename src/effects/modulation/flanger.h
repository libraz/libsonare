#pragma once

/// @file flanger.h
/// @brief Stereo flanger: a loop closed around a modulated delay, behind the
///        same optional one-pole section a chorus puts in front of its own.
///
/// What separates the two types is the loop, and its sign is part of it: a
/// negative gain resonates at the odd half-multiples of the loop's delay where
/// a positive one resonates at the multiples, so the teeth of the two signs sit
/// halfway apart. `feedback` therefore carries its sign through to the delay's
/// input rather than being read as a depth. The pre-filter section is declared
/// in chorus.h, which is where the shape and the corner it shares with a chorus
/// are described; this type runs its own instance of it.

#include <array>
#include <vector>

#include "effects/modulation/chorus.h"
#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct FlangerConfig {
  float rate_hz = 0.25f;
  float depth_ms = 2.0f;
  float center_delay_ms = 3.0f;
  /// Gain of the loop around the delay, clamped to +-0.95 in process(). Negative
  /// values invert the loop's return, which moves the teeth by half a spacing.
  float feedback = 0.3f;
  float dry_wet = 0.5f;
  /// Corner of the pre-filter section, in hertz. Read only when the mode below
  /// engages it; a non-positive corner is out of domain and leaves it off.
  float pre_filter_hz = 0.0f;
  PreFilterMode pre_filter_mode = PreFilterMode::kOff;
};

class Flanger : public rt::ProcessorBase {
 public:
  explicit Flanger(FlangerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0; updates both LFOs in place)
  //   1 = depth_ms
  //   2 = center_delay_ms
  //   3 = feedback (clamped to [-0.95, 0.95] in process(); the sign is carried)
  //   4 = dry_wet
  //   5 = pre_filter_hz (re-derives the pole in place; the filter keeps its state)
  // Note: `pre_filter_mode` is not automatable -- it selects a structure rather
  // than scaling one, and changing it needs prepare().
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Returns the feedback path to rest once a non-finite value has reached it,
  /// once per block (see util/non_finite_state.h).
  void discard_non_finite() noexcept;

  FlangerConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<ModDelayLine, 2> delays_;
  std::array<Lfo, 2> lfos_;
  /// [L, R] pre-filter sections. One per channel, never shared with another
  /// instance: what the measurement calls one section is one design.
  std::array<PreFilter, 2> pre_filters_;
  std::array<float, 2> feedback_{{0.0f, 0.0f}};
  /// Set by process() when a delay tap came back non-finite, cleared by
  /// discard_non_finite(). The poison is resident in the line rather than in a
  /// cell -- a tap reads it for a sample or two per lap and the cell is finite
  /// again by the end of the block -- so the block latches it as it passes.
  bool feedback_non_finite_ = false;
};

}  // namespace sonare::effects::modulation
