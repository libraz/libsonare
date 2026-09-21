#pragma once

/// @file pitch_shifter.h
/// @brief Real-time time-domain pitch shifter (dual crossfaded delay taps).

#include <array>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::effects::modulation {

/// The processor splices twice per grain, so the drift between splices -- the
/// window -- is half the grain span.
constexpr float kDefaultWindowMs = 22.5f;

struct PitchShifterConfig {
  float semitones = 0.0f;  ///< shift amount; +12 = one octave up.
  float dry_wet = 1.0f;
  /// Distance the read-out drifts between splices, in milliseconds, and the
  /// distance the two taps sit apart in the delay line. The output repeats once
  /// per window of drift, so its beat period is `window_ms / |ratio - 1|`.
  float window_ms = kDefaultWindowMs;
};

/// A classic H910-style pitch shifter: a delay line read by two taps one window
/// apart, each tap's delay ramped at the pitch ratio and the two crossfaded by
/// an equal-power grain so the wrap discontinuity is masked. Each tap wraps once
/// per grain and they are staggered by half of one, so the splice rate -- and
/// with it the audible beat -- is one per window of drift.
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
  int grain_ = 2048;    ///< grain length in samples: two of config_.window_ms.
  float phase_ = 0.0f;  ///< tap-1 delay position in [0, grain_).
  std::array<std::vector<float>, 2> buffers_;
  std::array<int, 2> write_pos_{{0, 0}};
};

}  // namespace sonare::effects::modulation
