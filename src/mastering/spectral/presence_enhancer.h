#pragma once

#include "mastering/common/processor_base.h"

namespace sonare::mastering::spectral {

struct PresenceEnhancerConfig {
  float amount = 0.2f;
  float drive = 2.0f;
};

class PresenceEnhancer : public common::ProcessorBase {
 public:
  explicit PresenceEnhancer(PresenceEnhancerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override {}
  void set_config(const PresenceEnhancerConfig& config);

 private:
  static void validate_config(const PresenceEnhancerConfig& config);
  PresenceEnhancerConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::spectral
