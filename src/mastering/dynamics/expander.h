#pragma once

/// @file expander.h
/// @brief Downward expander.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct ExpanderConfig {
  float threshold_db = -40.0f;
  float ratio = 2.0f;
  float attack_ms = 5.0f;
  float release_ms = 100.0f;
  float range_db = -60.0f;
};

class Expander : public common::ProcessorBase {
 public:
  explicit Expander(ExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const ExpanderConfig& config);
  const ExpanderConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const ExpanderConfig& config);
  static float linear_to_db(float value);
  static float db_to_linear(float db);
  static float gain_reduction_db(float input_db, const ExpanderConfig& config);
  void ensure_followers(int num_channels);

  ExpanderConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
