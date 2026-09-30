#pragma once

/// @file ring_modulator.h
/// @brief Ring modulator (input multiplied by a sinusoidal carrier).

#include <vector>

#include "effects/modulation/lfo.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct RingModulatorConfig {
  float carrier_hz = 200.0f;
  float dry_wet = 1.0f;
  /// Carrier waveform; sine is the classic ring modulator.
  LfoShape shape = LfoShape::kSine;
  /// Phase of the right carrier ahead of the left one, in degrees.
  float phase_deg = 0.0f;
  /// 0-1: how far the right carrier is additionally turned toward antiphase
  /// (half a turn at 1), on top of `phase_deg`.
  float stereo_spread = 0.0f;
};

/// Multiplies the signal by a sine carrier, producing the sum/difference
/// sidebands of a classic ring modulator. With `phase_deg` and `stereo_spread`
/// at zero the carrier phase is shared across channels, which keeps the stereo
/// image coherent.
class RingModulator : public rt::ProcessorBase {
 public:
  explicit RingModulator(RingModulatorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = carrier_hz (clamped to >= 0)
  //   1 = dry_wet (clamped to [0, 1] in process())
  //   2 = shape (LfoShape; a fractional or unnamed value is refused)
  //   3 = phase_deg (clamped to [0, 360])
  //   4 = stereo_spread (clamped to [0, 1])
  bool set_parameter_impl(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  RingModulatorConfig config_{};
  double sample_rate_ = 48000.0;
  double phase_ = 0.0;
};

}  // namespace sonare::effects::modulation
