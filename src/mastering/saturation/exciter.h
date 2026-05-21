#pragma once

#include <vector>

#include "mastering/common/processor_base.h"

namespace sonare::mastering::saturation {

struct ExciterConfig {
  float frequency_hz = 3000.0f;
  float drive_db = 6.0f;
  float amount = 0.25f;
};

class Exciter : public common::ProcessorBase {
 public:
  explicit Exciter(ExciterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const ExciterConfig& config);
  const ExciterConfig& config() const { return config_; }

 private:
  static void validate_config(const ExciterConfig& config);
  static float db_to_linear(float db);
  void update_coeff();
  void ensure_state(int num_channels);

  ExciterConfig config_{};
  double sample_rate_ = 48000.0;
  float lowpass_coeff_ = 0.0f;
  bool prepared_ = false;
  std::vector<float> lowpass_state_;
};

}  // namespace sonare::mastering::saturation
