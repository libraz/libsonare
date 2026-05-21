#pragma once

/// @file limiter.h
/// @brief Lookahead peak limiter.

#include <vector>

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
  const LimiterConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }
  int latency_samples() const { return lookahead_samples_; }

 private:
  static void validate_config(const LimiterConfig& config);
  static float db_to_linear(float db);
  static float linear_to_db(float value);
  void prepare_buffers(int num_channels);

  LimiterConfig config_{};
  double sample_rate_ = 48000.0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  std::vector<common::LookaheadBuffer> lookahead_;
  std::vector<float> gains_;
  float release_coeff_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
