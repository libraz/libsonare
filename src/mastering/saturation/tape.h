#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct TapeConfig {
  float drive_db = 3.0f;
  float saturation = 0.5f;
  float hysteresis = 0.2f;
  float output_gain_db = 0.0f;
};

class Tape : public common::ProcessorBase {
 public:
  explicit Tape(TapeConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TapeConfig& config);
  const TapeConfig& config() const { return config_; }

 private:
  static void validate_config(const TapeConfig& config);
  static float db_to_linear(float db);
  void ensure_state(int num_channels);

  TapeConfig config_{};
  bool prepared_ = false;
  std::vector<float> hysteresis_state_;
};

}  // namespace sonare::mastering::saturation
