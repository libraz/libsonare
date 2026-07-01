#pragma once

/// @file pitch_shifter.h
/// @brief Real-time time-domain pitch shifter (dual crossfaded delay taps).

#include <array>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct PitchShifterConfig {
  float semitones = 0.0f;  ///< shift amount; +12 = one octave up.
  float dry_wet = 1.0f;
};

/// A classic H910-style pitch shifter: a delay line read by two taps a
/// half-window apart, each tap's delay ramped at the pitch ratio and the two
/// crossfaded by an equal-power window so the wrap discontinuity is masked.
/// No FFT, so it is realtime-safe and low-latency (offline, higher-quality
/// spectral pitch shifting lives in effects/pitch_shift.h).
class PitchShifter : public rt::ProcessorBase {
 public:
  explicit PitchShifter(PitchShifterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, in-place scalar updates):
  //   0 = semitones (clamped to [-24, 24])
  //   1 = dry_wet
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  float read_tap(int channel, float delay) const noexcept;

  PitchShifterConfig config_{};
  double sample_rate_ = 48000.0;
  int window_ = 2048;   ///< grain window length in samples.
  float phase_ = 0.0f;  ///< tap-1 delay position in [0, window_).
  std::array<std::vector<float>, 2> buffers_;
  std::array<int, 2> write_pos_{{0, 0}};
};

}  // namespace sonare::effects::modulation
