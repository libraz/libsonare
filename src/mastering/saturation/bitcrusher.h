#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mastering/common/processor_base.h"
#include "mastering/final/dither.h"

namespace sonare::mastering::saturation {

struct BitCrusherConfig {
  int bit_depth = 12;
  int downsample_factor = 1;
  float mix = 1.0f;
  final::DitherType dither_type = final::DitherType::None;
  uint32_t dither_seed = 0x51A7E5u;
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
  float quantize(float sample, int bit_depth, int channel);
  float dither_noise(int channel);
  void ensure_state(int num_channels);

  BitCrusherConfig config_{};
  bool prepared_ = false;
  std::vector<float> held_;
  std::vector<int> counters_;
  std::vector<uint32_t> rng_state_;
  std::vector<std::array<float, 9>> error_history_;
};

}  // namespace sonare::mastering::saturation
