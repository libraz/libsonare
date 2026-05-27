#pragma once

/// @file ducking_processor.h
/// @brief Thin sidechain ducking wrapper for mix/voiceover workflows.

#include "mastering/dynamics/sidechain_router.h"

namespace sonare::mastering::dynamics {

struct DuckingConfig {
  float threshold_db = -18.0f;
  float ratio = 8.0f;
  float attack_ms = 2.0f;
  float release_ms = 150.0f;
  float range_db = 18.0f;
  float lookahead_ms = 0.0f;
};

class DuckingProcessor : public common::ProcessorBase {
 public:
  explicit DuckingProcessor(DuckingConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;

  void set_key_input(const float* const* channels, int num_channels, int num_samples);
  void clear_key_input();

  void set_config(const DuckingConfig& config);
  const DuckingConfig& config() const noexcept { return config_; }
  float last_gain_reduction_db() const noexcept override {
    return router_.last_gain_reduction_db();
  }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = range_db (clamped to >= 0)
  // lookahead_ms is omitted because changing it resizes the lookahead buffers.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static SidechainRouterConfig to_router_config(const DuckingConfig& config);

  DuckingConfig config_{};
  SidechainRouter router_;
  double sample_rate_ = 48000.0;
};

}  // namespace sonare::mastering::dynamics
