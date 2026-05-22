#pragma once

/// @file upward_expander.h
/// @brief Upward expander that emphasizes material above a threshold.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct UpwardExpanderConfig {
  float threshold_db = -24.0f;
  float ratio = 1.5f;
  float attack_ms = 5.0f;
  float release_ms = 80.0f;
  float range_db = 12.0f;
};

class UpwardExpander : public common::ProcessorBase {
 public:
  explicit UpwardExpander(UpwardExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const UpwardExpanderConfig& config);
  const UpwardExpanderConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

 private:
  static void validate_config(const UpwardExpanderConfig& config);
  static float gain_db(float input_db, const UpwardExpanderConfig& config);
  void ensure_followers(int num_channels);

  UpwardExpanderConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
