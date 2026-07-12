#pragma once

/// @file upward_expander.h
/// @brief Upward expander that emphasizes material above a threshold.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct UpwardExpanderConfig {
  float threshold_db = -24.0f;
  float ratio = 1.5f;
  float attack_ms = 5.0f;
  float release_ms = 80.0f;
  float range_db = 12.0f;
};

class UpwardExpander : public rt::ProcessorBase,
                       public rt::RtConfigLifecycle<UpwardExpander, UpwardExpanderConfig> {
  using ConfigBase = rt::RtConfigLifecycle<UpwardExpander, UpwardExpanderConfig>;
  friend ConfigBase;

 public:
  explicit UpwardExpander(UpwardExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (UpwardExpander::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (envelope followers) are
  // recomputed on the audio thread when the snapshot is adopted, so no
  // per-channel state is written concurrently with processing.
  float last_gain_db() const { return last_gain_db_; }
  float last_gain_reduction_db() const override { return last_gain_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = range_db (clamped to >= 0)
  //
  // set_parameter mutates the control-thread mirror (config_) directly and is
  // declared RT-safe. It MUST NOT be called concurrently with set_config(); the
  // single-producer hand-off contract of RtPublisher covers either path
  // individually, not both at once.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=ratio, 2=attackMs, 3=releaseMs, 4=rangeDb
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const UpwardExpanderConfig& config);
  static float gain_db(float input_db, const UpwardExpanderConfig& config);
  /// @brief Recomputes scalar derived coefficients (envelope followers) from
  ///        @p config. RT-safe: scalar math only, no allocation.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const UpwardExpanderConfig& config);
  void ensure_followers(int num_channels);

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  float last_gain_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
