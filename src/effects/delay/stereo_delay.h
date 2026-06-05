#pragma once

/// @file stereo_delay.h
/// @brief Zero-latency stereo feedback delay.

#include <array>

#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::delay {

struct StereoDelayConfig {
  float delay_time_l_ms = 250.0f;
  float delay_time_r_ms = 250.0f;
  float feedback = 0.25f;
  float ping_pong = 0.0f;
  float dry_wet = 0.5f;
};

class StereoDelay : public rt::ProcessorBase {
 public:
  explicit StereoDelay(StereoDelayConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const StereoDelayConfig& config) noexcept;
  const StereoDelayConfig& config() const noexcept { return config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = delay_time_l_ms
  //   1 = delay_time_r_ms
  //   2 = feedback (clamped to [0, 0.95] in process())
  //   3 = ping_pong (clamped to [0, 1] in process())
  //   4 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  StereoDelayConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<modulation::ModDelayLine, 2> delays_;
  std::array<float, 2> delay_samples_{{0.0f, 0.0f}};
  std::array<float, 2> feedback_state_{{0.0f, 0.0f}};
};

}  // namespace sonare::effects::delay
