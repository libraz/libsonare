#pragma once

/// @file phase_align.h
/// @brief Integer-sample stereo phase alignment delay.

#include <vector>

#include "rt/processor_base.h"

namespace sonare::mastering::stereo {

struct PhaseAlignConfig {
  int delay_samples = 0;
  bool delay_right = true;
  float fractional_delay_samples = 0.0f;
};

class PhaseAlign : public rt::ProcessorBase {
 public:
  explicit PhaseAlign(PhaseAlignConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_config(const PhaseAlignConfig& config);
  const PhaseAlignConfig& config() const { return config_; }
  static float estimate_delay_samples(const float* reference, const float* target, int num_samples,
                                      int max_abs_delay);

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = fractional_delay_samples (clamped to [0, 1); the whole-sample delay
  //       and therefore the delay-line size are unchanged, so the Lagrange
  //       interpolator simply reads the new fraction on the next sample)
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const PhaseAlignConfig& config);
  void rebuild_delay();
  float process_delay(float input);
  float total_delay_samples() const noexcept;

  PhaseAlignConfig config_{};
  size_t delay_index_ = 0;
  std::vector<float> delay_;
  bool prepared_ = false;
};

}  // namespace sonare::mastering::stereo
