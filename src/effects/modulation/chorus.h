#pragma once

/// @file chorus.h
/// @brief Stereo chorus built from modulated fractional delays.

#include <array>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct ChorusConfig {
  float rate_hz = 0.8f;
  float depth_ms = 6.0f;
  float center_delay_ms = 14.0f;
  float dry_wet = 0.5f;
};

class Chorus : public rt::ProcessorBase {
 public:
  explicit Chorus(ChorusConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0; updates both LFOs in place)
  //   1 = depth_ms
  //   2 = center_delay_ms
  //   3 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  ChorusConfig config_{};
  double sample_rate_ = 48000.0;
  std::array<ModDelayLine, 2> delays_;
  std::array<Lfo, 2> lfos_;
};

}  // namespace sonare::effects::modulation
