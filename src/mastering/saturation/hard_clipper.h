#pragma once

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct HardClipperConfig {
  float ceiling = 1.0f;
};

class HardClipper : public common::ProcessorBase {
 public:
  explicit HardClipper(HardClipperConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const HardClipperConfig& config);
  const HardClipperConfig& config() const { return config_; }

 private:
  static void validate_config(const HardClipperConfig& config);
  HardClipperConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::saturation
