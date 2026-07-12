#pragma once

/// @file expander.h
/// @brief Downward expander.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct ExpanderConfig {
  float threshold_db = -40.0f;
  float ratio = 2.0f;
  float attack_ms = 5.0f;
  float release_ms = 100.0f;
  float range_db = -60.0f;
};

class Expander : public rt::ProcessorBase, public rt::RtConfigLifecycle<Expander, ExpanderConfig> {
  using ConfigBase = rt::RtConfigLifecycle<Expander, ExpanderConfig>;
  friend ConfigBase;

 public:
  explicit Expander(ExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (Expander::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope followers) are
  // recomputed on the audio thread when the snapshot is adopted, so no
  // per-channel state is written concurrently with processing.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = range_db (clamped to <= 0)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=ratio, 2=attackMs, 3=releaseMs, 4=rangeDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const ExpanderConfig& config);
  static float gain_reduction_db(float input_db, const ExpanderConfig& config);
  void ensure_followers(int num_channels);
  /// @brief Recomputes scalar derived coefficients (envelope follower
  ///        attack/release) from @p config. RT-safe: scalar math only, no
  ///        allocation (envelope followers are preallocated in prepare()).
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const ExpanderConfig& config);

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
