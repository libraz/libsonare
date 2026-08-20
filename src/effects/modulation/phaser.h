#pragma once

/// @file phaser.h
/// @brief Stereo first-order allpass phaser.

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct PhaserConfig {
  float rate_hz = 0.4f;
  float min_hz = 300.0f;
  float max_hz = 1600.0f;
  int stages = 4;
  float dry_wet = 0.5f;
};

class Phaser : public rt::ProcessorBase {
 public:
  explicit Phaser(PhaserConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_hz (clamped to >= 0; updates the LFO in place)
  //   1 = min_hz (sweep lower bound)
  //   2 = max_hz (sweep upper bound)
  //   3 = dry_wet
  // Note: `stages` is not automatable; changing it reallocates the allpass
  // state and requires prepare().
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Allpass coefficient for one channel's current LFO value, mapping the
  /// oscillator's [-1, 1] output onto the configured sweep range.
  float sweep_coeff(float lfo_value) const noexcept;
  float process_channel(float input, int channel, float coeff);

  PhaserConfig config_{};
  double sample_rate_ = 48000.0;
  /// [L, R] sweep oscillators, offset by a quarter cycle in reset() so the two
  /// channels' notches never sit on the same frequencies (as in Chorus/Flanger).
  std::array<Lfo, 2> lfos_;
  std::array<std::vector<float>, 2> x1_;
  std::array<std::vector<float>, 2> y1_;
};

}  // namespace sonare::effects::modulation
