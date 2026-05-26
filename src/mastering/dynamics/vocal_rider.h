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
  float gain_smoothing_ms = 100.0f;
  float noise_floor_db = -60.0f;
  bool linked_detection = true;
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

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = target_db
  //   1 = max_boost_db (clamped to >= 0)
  //   2 = max_cut_db (clamped to >= 0)
  //   3 = attack_ms (clamped to >= 0)
  //   4 = release_ms (clamped to >= 0)
  //   5 = output_gain_db
  //   6 = gain_smoothing_ms (clamped to >= 0)
  //   7 = noise_floor_db
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const VocalRiderConfig& config);
  void ensure_followers(int num_channels);

  VocalRiderConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<common::EnvelopeFollower> followers_;
  float linked_gain_state_db_ = 0.0f;
  // Per-channel smoothed gain state for the unlinked path, persisted across
  // blocks so toggling linked/unlinked does not introduce a discontinuity.
  std::vector<float> unlinked_gain_state_db_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
