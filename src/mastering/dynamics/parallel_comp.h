#pragma once

/// @file parallel_comp.h
/// @brief Parallel compressor with dry/wet blend.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct ParallelCompConfig {
  float threshold_db = -18.0f;
  float ratio = 4.0f;
  float attack_ms = 10.0f;
  float release_ms = 100.0f;
  float makeup_gain_db = 0.0f;
  float mix = 0.5f;
};

class ParallelComp : public common::ProcessorBase {
 public:
  explicit ParallelComp(ParallelCompConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const ParallelCompConfig& config);
  const ParallelCompConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const ParallelCompConfig& config);
  static float linear_to_db(float value);
  static float db_to_linear(float db);
  static float gain_reduction_db(float input_db, const ParallelCompConfig& config);
  void ensure_followers(int num_channels);

  ParallelCompConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
