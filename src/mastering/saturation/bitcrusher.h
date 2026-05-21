#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct BitCrusherConfig {
  int bit_depth = 12;
  int downsample_factor = 1;
  float mix = 1.0f;
};

class BitCrusher : public common::ProcessorBase {
 public:
  explicit BitCrusher(BitCrusherConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const BitCrusherConfig& config);
  const BitCrusherConfig& config() const { return config_; }

 private:
  static void validate_config(const BitCrusherConfig& config);
  static float quantize(float sample, int bit_depth);
  void ensure_state(int num_channels);

  BitCrusherConfig config_{};
  bool prepared_ = false;
  std::vector<float> held_;
  std::vector<int> counters_;
};

}  // namespace sonare::mastering::saturation
