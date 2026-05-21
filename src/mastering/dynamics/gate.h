#pragma once

/// @file gate.h
/// @brief Noise gate built on the expander curve.

#include "mastering/common/processor_base.h"
#include "mastering/dynamics/expander.h"

namespace sonare::mastering::dynamics {

struct GateConfig {
  float threshold_db = -50.0f;
  float attack_ms = 2.0f;
  float release_ms = 80.0f;
  float range_db = -80.0f;
};

class Gate : public common::ProcessorBase {
 public:
  explicit Gate(GateConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const GateConfig& config);
  const GateConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return expander_.last_gain_reduction_db(); }

 private:
  static void validate_config(const GateConfig& config);

  GateConfig config_{};
  Expander expander_;
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
};

}  // namespace sonare::mastering::dynamics
