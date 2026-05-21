#pragma once

/// @file haas_enhancer.h
/// @brief Haas-delay stereo widener.

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::stereo {

struct HaasEnhancerConfig {
  float delay_ms = 12.0f;
  float mix = 1.0f;
  bool delay_right = true;
};

class HaasEnhancer : public common::ProcessorBase {
 public:
  explicit HaasEnhancer(HaasEnhancerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const HaasEnhancerConfig& config);
  const HaasEnhancerConfig& config() const { return config_; }
  int delay_samples() const { return delay_samples_; }

 private:
  static void validate_config(const HaasEnhancerConfig& config);
  void rebuild_delay();
  float process_delay(float input);

  HaasEnhancerConfig config_{};
  double sample_rate_ = 48000.0;
  int delay_samples_ = 0;
  size_t delay_index_ = 0;
  std::vector<float> delay_;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
