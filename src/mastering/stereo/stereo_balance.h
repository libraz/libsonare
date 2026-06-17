#pragma once

/// @file stereo_balance.h
/// @brief Left/right balance and pan style gain processor.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

struct StereoBalanceConfig {
  float balance = 0.0f;
  bool constant_power = true;
};

class StereoBalance : public rt::ProcessorBase {
 public:
  explicit StereoBalance(StereoBalanceConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const StereoBalanceConfig& config);
  const StereoBalanceConfig& config() const { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = balance (clamped to [-1, 1])
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=balance
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const StereoBalanceConfig& config);
  static void gains(const StereoBalanceConfig& config, float& left, float& right);

  StereoBalanceConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
