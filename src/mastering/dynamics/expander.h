#pragma once

/// @file expander.h
/// @brief Downward expander.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct ExpanderConfig {
  float threshold_db = -40.0f;
  float ratio = 2.0f;
  float attack_ms = 5.0f;
  float release_ms = 100.0f;
  float range_db = -60.0f;
};

class Expander : public rt::ProcessorBase {
 public:
  explicit Expander(ExpanderConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (envelope followers) are recomputed on the
  ///          audio thread when the snapshot is adopted, so no per-channel
  ///          state member is ever written concurrently with sample
  ///          processing. May allocate (the snapshot @c shared_ptr) and is
  ///          therefore NOT realtime-safe itself; call from the configuration
  ///          thread only. Two threads MUST NOT call @ref set_config
  ///          concurrently with each other (single-producer hand-off). Throws
  ///          @c std::invalid_argument with the same rules as the constructor;
  ///          on throw the published configuration is unchanged (validation
  ///          happens before publish, never partway).
  void set_config(const ExpanderConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const ExpanderConfig& config() const { return config_; }
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
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const ExpanderConfig* adopt_snapshot_for_block() noexcept;

  ExpanderConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. Held by @c unique_ptr so Expander
  ///        itself remains move-constructible (RtPublisher deletes its copy
  ///        and move operations to keep the SPSC ring indices position-
  ///        stable); the indirection is touched once per block from the audio
  ///        thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<ExpanderConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const ExpanderConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::vector<sonare::rt::EnvelopeFollower> followers_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
