#pragma once

#include "mastering/maximizer/true_peak_limiter.h"

namespace sonare::mastering::maximizer {

struct AdaptiveReleaseConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float min_release_ms = 20.0f;
  float max_release_ms = 250.0f;
};

class AdaptiveRelease : public common::ProcessorBase {
 public:
  explicit AdaptiveRelease(AdaptiveReleaseConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const AdaptiveReleaseConfig& config);
  const AdaptiveReleaseConfig& config() const { return config_; }
  float current_release_ms() const { return current_release_ms_; }
  float last_gain_reduction_db() const { return limiter_.last_gain_reduction_db(); }

 private:
  static void validate_config(const AdaptiveReleaseConfig& config);
  void configure_limiter();

  AdaptiveReleaseConfig config_{};
  TruePeakLimiter limiter_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  float current_release_ms_ = 20.0f;
};

}  // namespace sonare::mastering::maximizer
