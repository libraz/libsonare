#pragma once

/// @file mono_maker.h
/// @brief Stereo to mono low-frequency utility.

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

struct MonoMakerConfig {
  float amount = 1.0f;
};

class MonoMaker : public rt::ProcessorBase {
 public:
  explicit MonoMaker(MonoMakerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const MonoMakerConfig& config);
  const MonoMakerConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = amount (clamped to [0, 1])
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const MonoMakerConfig& config);

  MonoMakerConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
