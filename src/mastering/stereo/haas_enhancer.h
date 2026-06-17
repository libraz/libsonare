#pragma once

/// @file haas_enhancer.h
/// @brief Haas-delay stereo widener.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

struct HaasEnhancerConfig {
  float delay_ms = 12.0f;
  float mix = 1.0f;
  bool delay_right = true;
};

class HaasEnhancer : public rt::ProcessorBase {
 public:
  explicit HaasEnhancer(HaasEnhancerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const HaasEnhancerConfig& config);
  const HaasEnhancerConfig& config() const { return config_; }
  int delay_samples() const { return delay_samples_; }

  // Automatable parameters:
  //   0 = delay_ms (clamped to >= 0; reallocates the delay line and clears its
  //       state when prepared, so this id is NOT realtime-safe)
  //   1 = mix (clamped to [0, 1], RT-safe, no state reset)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=delayMs, 1=mix
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

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
