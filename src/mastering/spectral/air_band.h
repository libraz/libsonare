#pragma once

#include "mastering/common/processor_base.h"

namespace sonare::mastering::spectral {

struct AirBandConfig {
  float amount = 0.25f;
};

class AirBand : public common::ProcessorBase {
 public:
  explicit AirBand(AirBandConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override {}
  void set_config(const AirBandConfig& config);

 private:
  static void validate_config(const AirBandConfig& config);
  AirBandConfig config_{};
  bool prepared_ = false;
};

}  // namespace sonare::mastering::spectral
