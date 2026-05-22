#pragma once

#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct TransformerConfig {
  float drive_db = 4.0f;
  float asymmetry = 0.1f;
  float mix = 1.0f;
};

class Transformer : public common::ProcessorBase {
 public:
  explicit Transformer(TransformerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TransformerConfig& config);
  const TransformerConfig& transformer_config() const { return transformer_config_; }

 private:
  static void validate_config(const TransformerConfig& config);
  static common::JilesAthertonConfig make_ja_config(const TransformerConfig& config);
  float process_sample(common::JilesAthertonState& state, float input) const;
  void ensure_state(int num_channels);

  TransformerConfig transformer_config_{};
  common::JilesAtherton hysteresis_;
  bool prepared_ = false;
  std::vector<common::JilesAthertonState> states_;
};

}  // namespace sonare::mastering::saturation
