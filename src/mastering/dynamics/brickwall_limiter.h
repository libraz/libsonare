#pragma once

/// @file brickwall_limiter.h
/// @brief Hard-ceiling limiter that guarantees sample peaks do not exceed the ceiling.

#include <memory>
#include <vector>

#include "mastering/dynamics/limiter.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct BrickwallLimiterConfig {
  float ceiling_db = -1.0f;
  float lookahead_ms = 1.0f;
  float release_ms = 50.0f;
};

class BrickwallLimiter : public rt::ProcessorBase,
                         public rt::RtConfigLifecycle<BrickwallLimiter, BrickwallLimiterConfig> {
  using ConfigBase = rt::RtConfigLifecycle<BrickwallLimiter, BrickwallLimiterConfig>;
  friend ConfigBase;

 public:
  explicit BrickwallLimiter(BrickwallLimiterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance
  ///          for @c ceiling_db and @c release_ms updates only: those fields
  ///          are picked up by the audio thread from a lock-free snapshot
  ///          (see @c rt::RtPublisher) at the start of the next block and
  ///          applied via RT-safe scalar coefficient updates on the inner
  ///          @ref Limiter. Changing @c lookahead_ms requires resizing the
  ///          lookahead ring buffers, which is NOT realtime-safe; that branch
  ///          re-runs @ref prepare on the control thread and MUST NOT race
  ///          with @ref process. May allocate (the snapshot @c shared_ptr,
  ///          and the buffer resize on lookahead changes) and is therefore
  ///          NOT realtime-safe itself; call from the configuration thread
  ///          only. Two threads MUST NOT call @ref set_config concurrently
  ///          with each other (single-producer hand-off). Throws
  ///          @c std::invalid_argument with the same rules as the
  ///          constructor; on throw the published configuration is unchanged
  ///          (validation happens before publish, never partway).
  void set_config(const BrickwallLimiterConfig& config);
  void set_release_ms(float release_ms);
  /// @brief Realtime-safe release update for per-block automation.
  /// @details Forwards to @ref Limiter::set_release_ms_in_place on the inner
  ///          limiter, recomputing the release coefficient in place without
  ///          publishing a configuration snapshot (no allocation). Safe to call
  ///          once per block from the audio thread. Does NOT update the
  ///          control-thread @c config_ mirror or the published snapshot. Uses
  ///          the same release-coefficient math as @ref set_release_ms.
  void set_release_ms_in_place(float release_ms) noexcept;
  // config() is provided by RtConfigLifecycle; set_config() is overridden below
  // because a lookahead_ms change must resize buffers (not RT-safe) instead of
  // going through the lock-free publisher.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }
  int hard_clip_count() const noexcept { return hard_clip_count_; }
  int latency_samples() const noexcept override { return limiter_.latency_samples(); }

  // RT-safe: set_parameter updates the audio thread's live working config
  // (active_) in place — the hard-clip stage reads active_.ceiling_db and the
  // inner limiter's threshold/release are forwarded via its in-place setters —
  // without publishing a snapshot (no allocation), so it is safe to apply from
  // the audio callback. The control-thread mirror (config_) is kept in sync so
  // config() reads back the automated state; only the snapshot publish (the
  // allocation) is dropped.
  //   0 = ceiling_db
  //   1 = release_ms (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=ceilingDb, 1=releaseMs
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const BrickwallLimiterConfig& config);
  /// @brief Recomputes scalar derived coefficients (forwards ceiling and
  ///        release to the inner @ref Limiter via its RT-safe parameter
  ///        setters) from @p config. RT-safe: scalar math only, no
  ///        allocation, no buffer resize.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks. Never resizes the lookahead buffers; a
  ///          changed @c lookahead_ms is handled by the control-thread
  ///          @ref set_config branch that re-runs @ref prepare.
  void update_coefficients(const BrickwallLimiterConfig& config);

  Limiter limiter_;
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float last_gain_reduction_db_ = 0.0f;
  int hard_clip_count_ = 0;
};

}  // namespace sonare::mastering::dynamics
