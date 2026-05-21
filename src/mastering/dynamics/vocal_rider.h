#pragma once

/// @file vocal_rider.h
/// @brief Automatic level rider that moves signal toward a target loudness.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct VocalRiderConfig {
  float target_db = -18.0f;
  float max_boost_db = 6.0f;
  float max_cut_db = 6.0f;
  float attack_ms = 50.0f;
  float release_ms = 500.0f;
  float output_gain_db = 0.0f;
};

class VocalRider : public common::ProcessorBase {
 public:
  explicit VocalRider(VocalRiderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const VocalRiderConfig& config);
  const VocalRiderConfig& config() const { return config_; }
  float last_gain_db() const { return last_gain_db_; }

 private:
  static void validate_config(const VocalRiderConfig& config);
  static float linear_to_db(float value);
  static float db_to_linear(float db);
  void ensure_followers(int num_channels);

  VocalRiderConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
