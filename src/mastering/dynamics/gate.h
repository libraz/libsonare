#pragma once

/// @file gate.h
/// @brief Noise gate built on the expander curve.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
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

class Gate : public rt::ProcessorBase, public rt::RtConfigLifecycle<Gate, GateConfig> {
  using ConfigBase = rt::RtConfigLifecycle<Gate, GateConfig>;
  friend ConfigBase;

 public:
  explicit Gate(GateConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (Gate::validate_config) before publishing a lock-free snapshot
  // the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (sidechain HPF) are recomputed
  // on the audio thread when the snapshot is adopted, so no per-channel state
  // is written concurrently with processing.
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
