#pragma once

/// @file true_peak_limiter.h
/// @brief Ceiling limiter with true-peak style post guard.

#include "mastering/dynamics/brickwall_limiter.h"

namespace sonare::mastering::maximizer {

struct TruePeakLimiterConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
  int oversample_factor = 4;
};

class TruePeakLimiter : public common::ProcessorBase {
 public:
  explicit TruePeakLimiter(TruePeakLimiterConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TruePeakLimiterConfig& config);
  const TruePeakLimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const TruePeakLimiterConfig& config);
  static float db_to_linear(float db);
  static float linear_to_db(float value);

  TruePeakLimiterConfig config_{};
  dynamics::BrickwallLimiter limiter_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::maximizer
