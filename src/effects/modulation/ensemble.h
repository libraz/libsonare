#pragma once

/// @file ensemble.h
/// @brief BBD-style string-machine ensemble (Solina-type 3-phase chorus).
///
/// The classic ensemble is three bucket-brigade delay lines per channel
/// modulated by a SLOW and a FAST LFO simultaneously, with the three taps
/// 120 degrees apart on both — the dual-rate 3-phase scheme is what turns a
/// single voice into a "section". Two BBD character traits are modelled on
/// the wet path: the limited bucket bandwidth (a gentle one-pole lowpass,
/// `tone_hz`) and the inherently mono-summed source spreading into stereo
/// (the right channel reads the same 3-phase pattern with inverted LFO
/// polarity).

#include <array>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "rt/processor_base.h"

namespace sonare::effects::modulation {

struct EnsembleConfig {
  /// Dual LFO rates (Hz): the slow "chorale" sweep and the fast shimmer.
  float rate_slow_hz = 0.6f;
  float rate_fast_hz = 5.5f;
  /// Modulation depths (ms) for each LFO.
  float depth_slow_ms = 1.8f;
  float depth_fast_ms = 0.25f;
  /// Nominal bucket delay (ms).
  float center_delay_ms = 5.0f;
  /// BBD bandwidth: one-pole lowpass on the wet path (Hz).
  float tone_hz = 6500.0f;
  float dry_wet = 0.5f;
};

class Ensemble : public rt::ProcessorBase {
 public:
  explicit Ensemble(EnsembleConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = rate_slow_hz   (>= 0)
  //   1 = rate_fast_hz   (>= 0)
  //   2 = depth_slow_ms  (clamped to [0, 10])
  //   3 = depth_fast_ms  (clamped to [0, 10])
  //   4 = center_delay_ms (clamped to [0, 25])
  //   5 = tone_hz        (clamped to [500, 20000])
  //   6 = dry_wet        (clamped to [0, 1])
  bool set_parameter(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  EnsembleConfig config_{};
  double sample_rate_ = 48000.0;
  /// 3 taps x 2 channels.
  std::array<ModDelayLine, 6> delays_;
  /// 3-phase (0 / 120 / 240 degree) slow and fast LFO banks.
  std::array<Lfo, 3> slow_lfos_;
  std::array<Lfo, 3> fast_lfos_;
  std::array<float, 2> tone_state_{};
};

}  // namespace sonare::effects::modulation
