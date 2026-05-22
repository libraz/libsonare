#pragma once

/// @file limiter.h
/// @brief Lookahead peak limiter.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/lookahead_buffer.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct LimiterConfig {
  float threshold_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
};

class Limiter : public common::ProcessorBase {
 public:
  explicit Limiter(LimiterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const LimiterConfig& config);
  void set_release_ms(float release_ms);
  const LimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }
  int latency_samples() const noexcept override { return lookahead_samples_; }

 private:
  static void validate_config(const LimiterConfig& config);
  void prepare_buffers(int num_channels);
  void update_release_coeff();

  LimiterConfig config_{};
  double sample_rate_ = 48000.0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  std::vector<common::LookaheadBuffer> lookahead_;
  std::vector<common::EnvelopeFollower> gain_smoothers_;
  float release_coeff_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
