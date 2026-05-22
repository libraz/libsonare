#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::spectral {

struct LowEndFocusConfig {
  float cutoff_hz = 150.0f;
  float width = 0.0f;
  float subharmonic_amount = 0.0f;
  float transient_tightness = 0.0f;
};

class LowEndFocus : public common::ProcessorBase {
 public:
  explicit LowEndFocus(LowEndFocusConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const LowEndFocusConfig& config);

 private:
  static void validate_config(const LowEndFocusConfig& config);
  LowEndFocusConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<float> low_state_;
  std::vector<float> sub_state_;
  std::vector<float> transient_state_;
  std::vector<float> previous_low_;
  std::vector<float> divider_polarity_;
};

}  // namespace sonare::mastering::spectral
