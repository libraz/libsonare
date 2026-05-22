#pragma once

#include "mastering/maximizer/maximizer.h"

namespace sonare::mastering::maximizer {

struct SoftKneeMaxConfig {
  float input_gain_db = 0.0f;
  float ceiling_db = -1.0f;
  float knee_db = 6.0f;
  float release_ms = 50.0f;
};

class SoftKneeMax : public common::ProcessorBase {
 public:
  explicit SoftKneeMax(SoftKneeMaxConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const SoftKneeMaxConfig& config);
  const SoftKneeMaxConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return maximizer_.last_gain_reduction_db(); }

 private:
  static void validate_config(const SoftKneeMaxConfig& config);

  SoftKneeMaxConfig config_{};
  Maximizer maximizer_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::maximizer
