#pragma once

/// @file stereo_balance.h
/// @brief Left/right balance and pan style gain processor.

#include "mastering/common/processor_base.h"

namespace sonare::mastering::stereo {

struct StereoBalanceConfig {
  float balance = 0.0f;
  bool constant_power = true;
};

class StereoBalance : public common::ProcessorBase {
 public:
  explicit StereoBalance(StereoBalanceConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const StereoBalanceConfig& config);
  const StereoBalanceConfig& config() const { return config_; }

 private:
  static void validate_config(const StereoBalanceConfig& config);
  static void gains(const StereoBalanceConfig& config, float& left, float& right);

  StereoBalanceConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
