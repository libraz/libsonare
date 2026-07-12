#pragma once

/// @file vocal_rider.h
/// @brief Automatic level rider that moves signal toward a target loudness.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

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

class VocalRider : public rt::ProcessorBase,
                   public rt::RtConfigLifecycle<VocalRider, VocalRiderConfig> {
  using ConfigBase = rt::RtConfigLifecycle<VocalRider, VocalRiderConfig>;
  friend ConfigBase;

 public:
  explicit VocalRider(VocalRiderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (VocalRider::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope follower attack/
  // release) are recomputed on the audio thread when the snapshot is adopted,
  // so no per-channel state is written concurrently with processing.
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
  // Automatable parameters: 0=targetDb, 1=maxBoostDb, 2=maxCutDb, 3=attackMs,
  //   4=releaseMs, 5=outputGainDb, 6=gainSmoothingMs, 7=noiseFloorDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const VocalRiderConfig& config);
  void ensure_followers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (envelope follower
  ///        attack/release) from @p config. RT-safe: scalar math only, no
  ///        allocation; the follower rewrites preserve envelope state.
  void update_coefficients(const VocalRiderConfig& config);

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<rt::EnvelopeFollower> followers_;
  float linked_gain_state_db_ = 0.0f;
  // Per-channel smoothed gain state for the unlinked path, persisted across
  // blocks so toggling linked/unlinked does not introduce a discontinuity.
  std::vector<float> unlinked_gain_state_db_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
