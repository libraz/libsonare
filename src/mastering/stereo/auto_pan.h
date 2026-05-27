#pragma once

/// @file auto_pan.h
/// @brief LFO-based stereo auto panner.

#include "mastering/common/processor_base.h"

namespace sonare::mastering::stereo {

struct AutoPanConfig {
  float rate_hz = 1.0f;
  float depth = 1.0f;
  float phase = 0.0f;
};

class AutoPan : public common::ProcessorBase {
 public:
  explicit AutoPan(AutoPanConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const AutoPanConfig& config);
  const AutoPanConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0)
  //   1 = depth (clamped to [0, 1])
  //   2 = phase
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const AutoPanConfig& config);

  AutoPanConfig config_{};
  double sample_rate_ = 48000.0;
  double phase_ = 0.0;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
