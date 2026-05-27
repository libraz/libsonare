#pragma once

/// @file flanger.h
/// @brief Stereo flanger with feedback.

#include <array>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct FlangerConfig {
  float rate_hz = 0.25f;
  float depth_ms = 2.0f;
  float center_delay_ms = 3.0f;
  float feedback = 0.3f;
  float dry_wet = 0.5f;
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
  //   3 = feedback (clamped to [-0.95, 0.95] in process())
  //   4 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  FlangerConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<ModDelayLine, 2> delays_;
  std::array<Lfo, 2> lfos_;
  std::array<float, 2> feedback_{{0.0f, 0.0f}};
};

}  // namespace sonare::effects::modulation
