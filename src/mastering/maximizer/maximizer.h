#pragma once

/// @file maximizer.h
/// @brief Input-gain maximizer with a hard output ceiling.

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/brickwall_limiter.h"

namespace sonare::mastering::maximizer {

struct MaximizerConfig {
  float input_gain_db = 0.0f;
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
};

class Maximizer : public common::ProcessorBase {
 public:
  explicit Maximizer(MaximizerConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const MaximizerConfig& config);
  const MaximizerConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return limiter_.last_gain_reduction_db(); }

 private:
  static void validate_config(const MaximizerConfig& config);

  MaximizerConfig config_{};
  dynamics::BrickwallLimiter limiter_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::maximizer
