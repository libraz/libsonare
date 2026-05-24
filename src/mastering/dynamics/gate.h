#pragma once

/// @file gate.h
/// @brief Noise gate built on the expander curve.

#include <vector>

#include "mastering/common/processor_base.h"

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

class Gate : public common::ProcessorBase {
 public:
  explicit Gate(GateConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const GateConfig& config);
  const GateConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

 private:
  static void validate_config(const GateConfig& config);
  static float coeff(double sample_rate, float ms);

  GateConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  float gain_db_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
  int hold_samples_remaining_ = 0;
  bool gate_open_ = false;
  float hpf_coeff_ = 0.0f;
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
};

}  // namespace sonare::mastering::dynamics
