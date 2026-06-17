#pragma once

/// @file gate.h
/// @brief Noise gate built on the expander curve.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

struct GateConfig {
  float threshold_db = -50.0f;
  float attack_ms = 2.0f;
  float release_ms = 80.0f;
  float range_db = -80.0f;
  float hold_ms = 0.0f;
  float close_threshold_db = -50.0f;
  float key_hpf_hz = 0.0f;
};

class Gate : public rt::ProcessorBase {
 public:
  explicit Gate(GateConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (sidechain HPF) are recomputed on the audio
  ///          thread when the snapshot is adopted, so no per-channel state
  ///          member is ever written concurrently with sample processing. May
  ///          allocate (the snapshot @c shared_ptr) and is therefore NOT
  ///          realtime-safe itself; call from the configuration thread only.
  ///          Two threads MUST NOT call @ref set_config concurrently with each
  ///          other (single-producer hand-off). Throws @c std::invalid_argument
  ///          with the same rules as the constructor; on throw the published
  ///          configuration is unchanged (validation happens before publish,
  ///          never partway).
  void set_config(const GateConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const GateConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // RT-safe: set_parameter updates the audio thread's live working config
  // (active_) in place and re-derives coefficients without publishing a snapshot
  // (no allocation), so it is safe to apply from the audio callback. The
  // control-thread mirror (config_) is kept in sync so config() reads back the
  // automated state; only the snapshot publish (the allocation) is dropped.
  //   0 = threshold_db (close_threshold_db kept <= threshold_db)
  //   1 = attack_ms (clamped to >= 0)
  //   2 = release_ms (clamped to >= 0)
  //   3 = range_db (clamped to <= 0)
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=attackMs, 2=releaseMs, 3=rangeDb.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const GateConfig& config);
  /// @brief Recomputes scalar derived coefficients (sidechain HPF) from
  ///        @p config. RT-safe: scalar math only, no allocation.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const GateConfig& config);
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const GateConfig* adopt_snapshot_for_block() noexcept;

  GateConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. Held by @c unique_ptr so Gate itself
  ///        remains move-constructible (RtPublisher deletes its copy and move
  ///        operations to keep the SPSC ring indices position-stable); the
  ///        indirection is touched once per block from the audio thread and
  ///        never per-sample.
  std::unique_ptr<rt::RtPublisher<GateConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const GateConfig* applied_snapshot_ = nullptr;
  /// @brief The audio thread's live working configuration. Seeded from the
  ///        adopted snapshot on each set_config change and mutated in place by
  ///        @ref set_parameter (RT-safe automation, no publish). The per-sample
  ///        loop reads this, not the snapshot.
  GateConfig active_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  // Smoothed gain in the linear (0..1) domain; 1.0 == unity (open).
  float gain_ = 1.0f;
  float last_gain_reduction_db_ = 0.0f;
  int hold_samples_remaining_ = 0;
  bool gate_open_ = false;
  float hpf_b0_ = 1.0f;
  float hpf_a1_ = 0.0f;
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
};

}  // namespace sonare::mastering::dynamics
