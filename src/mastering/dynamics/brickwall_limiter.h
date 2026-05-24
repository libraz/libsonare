#pragma once

/// @file brickwall_limiter.h
/// @brief Hard-ceiling limiter that guarantees sample peaks do not exceed the ceiling.

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/limiter.h"

namespace sonare::mastering::dynamics {

struct BrickwallLimiterConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
};

class BrickwallLimiter : public common::ProcessorBase {
 public:
  explicit BrickwallLimiter(BrickwallLimiterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const BrickwallLimiterConfig& config);
  void set_release_ms(float release_ms);
  const BrickwallLimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }
  int hard_clip_count() const noexcept { return hard_clip_count_; }
  int latency_samples() const noexcept override { return limiter_.latency_samples(); }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = ceiling_db
  //   1 = release_ms (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const BrickwallLimiterConfig& config);

  BrickwallLimiterConfig config_{};
  Limiter limiter_;
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float last_gain_reduction_db_ = 0.0f;
  int hard_clip_count_ = 0;
};

}  // namespace sonare::mastering::dynamics
