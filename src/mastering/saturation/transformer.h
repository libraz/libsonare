#pragma once

#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct TransformerConfig {
  float drive_db = 4.0f;
  float asymmetry = 0.1f;
  float mix = 1.0f;
};

class Transformer : public rt::ProcessorBase {
 public:
  explicit Transformer(TransformerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TransformerConfig& config);
  const TransformerConfig& transformer_config() const { return transformer_config_; }

  // Automatable parameters (RT-safe, no allocation, no audio-state reset):
  //   0 = drive_db (read per sample)
  //   1 = asymmetry (clamped to [-1, 1]; sets bias and updates J-A coercivity)
  //   2 = mix (clamped to [0, 1]; read per sample)
  // The J-A config update only touches coefficients; the per-channel
  // magnetization state in states_ is preserved.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=driveDb, 1=asymmetry, 2=mix
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

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
